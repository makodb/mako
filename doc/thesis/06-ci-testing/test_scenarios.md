# CI Test Scenarios

## 1. Overview

The Mako-Raft CI suite includes 5 integration test scenarios, each
invoked via `ci_mako_raft.sh` or `ci.sh`.  Tests progress from basic
Raft replication to full transactional workloads across multiple shards.

| # | Scenario | Binary | Shards | Replicas | Duration | Workload |
|---|----------|--------|--------|----------|----------|----------|
| 1 | simpleRaft | `simpleRaft` | 1 (3 partitions) | 3 | 40 s | 100 logs x 3 partitions |
| 2 | shard1ReplicationRaft | `dbtest` | 1 | 3 | 60 s | TPC-C (6 threads) |
| 3 | shard2ReplicationRaft | `dbtest` | 2 | 3 each | 120 s | TPC-C (6 threads) |
| 4 | shard1ReplicationSimpleRaft | `simpleTransactionRepRaft` | 1 | 3 | 40 s | Simple key-value |
| 5 | shard2ReplicationSimpleRaft | `simpleTransactionRepRaft` | 2 | 3 each | 60 s | Simple key-value |

## 2. Scenario 1: simpleRaft

**Script**: `examples/mako-raft-tests/simpleRaft.sh` (120 lines)

**Purpose**: Validates basic Raft log replication without any Mako
transaction processing.  This is the simplest possible Raft test —
a leader submits fixed-size log entries and followers replicate them.

### 2.1 Configuration

- **Binary**: `build/simpleRaft`
- **Replicas**: 3 (localhost = preferred leader, p1, p2)
- **Partitions**: 3 (per simpleRaft binary configuration)
- **Log entries**: 100 per partition = 300 total
- **Log size**: 3 KB each
- **Submission interval**: 5 ms between entries

### 2.2 Execution Sequence

```
1. Kill lingering simpleRaft processes
2. Start p1 (follower) → raft_a2.log      [background]
3. Sleep 2s
4. Start p2 (follower) → raft_a3.log      [background]
5. Sleep 2s
6. Start localhost (leader) → raft_a1.log  [background]
7. Sleep 40s (wait for completion)
8. Parse logs for callback counts
9. Kill all processes (SIGTERM + SIGKILL)
```

### 2.3 Pass/Fail Criteria

| Criterion | Threshold | Log File | Grep Pattern |
|-----------|-----------|----------|--------------|
| p1 follower callbacks | >= 300 | `raft_a2.log` | `RESULTS.*follower_callbacks=` |
| p2 follower callbacks | >= 300 | `raft_a3.log` | `RESULTS.*follower_callbacks=` |

The leader callback count is logged but not required for pass (leader
may hang during shutdown before printing results).

### 2.4 What It Tests

- Raft leader election in a 3-node cluster
- Preferred leader mechanism (localhost gets shorter election timeout)
- Log entry replication to both followers
- Callback invocation on commit

## 3. Scenario 2: shard1ReplicationRaft

**Script**: `examples/mako-raft-tests/test_1shard_replication_raft.sh` (153 lines)

**Purpose**: Runs TPC-C benchmark on a single shard with 3 Raft
replicas.  This tests Raft replication under real transactional workload
including NewOrder, Payment, and other TPC-C transactions.

### 3.1 Configuration

- **Binary**: `dbtest` (via `bash/shard_raft.sh`)
- **Shards**: 1
- **Replicas per shard**: 3 (localhost, p1, p2)
- **Threads**: 6 (configurable via `$1`, default 6)
- **Benchmark**: TPC-C
- **Duration**: 60 s
- **Config files**:
  - `config/1leader_2followers/raft6_shardidx0.yml` (site topology)
  - `config/occ_raft.yml` (mode: `cc:occ, ab:raft`)
  - `src/mako/config/local-shards1-warehouses6.yml` (shard config)

### 3.2 Execution Sequence

```
1. Kill lingering dbtest/simpleRaft processes
2. Clean old log files and RocksDB data
3. Start 3 replicas via shard_raft.sh 1 0 6 {localhost,p2,p1} 0 1
   - localhost first, p2 second, p1 last (1s delay between)
4. Sleep 60s
5. Kill processes
6. Parse logs
```

### 3.3 Log Files

| Log File | Process | Role |
|----------|---------|------|
| `test_1shard_replication_raft.sh_shard0-localhost-6.log` | `dbtest` | Leader |
| `test_1shard_replication_raft.sh_shard0-p1-6.log` | `dbtest` | Follower |
| `test_1shard_replication_raft.sh_shard0-p2-6.log` | `dbtest` | Follower |

### 3.4 Pass/Fail Criteria

| Criterion | Threshold | Log File | Grep Pattern |
|-----------|-----------|----------|--------------|
| Throughput reported | Present | leader log | `agg_persist_throughput` |
| Abort ratio | < 20% | leader log | `NewOrder_remote_abort_ratio:` |
| Follower replication | > 500 batches | p1 log | `replay_batch:` |

The `replay_batch` threshold was lowered from 1000 to 500 to account
for CI environment variability.  The test verifies replication is working,
not exact batch count.

### 3.5 What It Tests

- Raft replication under TPC-C transactional load
- Leader handles concurrent transactions while replicating
- Followers replay transaction batches
- OCC concurrency control works alongside Raft

## 4. Scenario 3: shard2ReplicationRaft

**Script**: `examples/mako-raft-tests/test_2shard_replication_raft.sh` (208 lines)

**Purpose**: Runs TPC-C benchmark on 2 shards, each with 3 Raft
replicas (6 total `dbtest` processes).  Tests cross-shard transactions
with Raft replication.

### 4.1 Configuration

- **Binary**: `dbtest` (via `bash/shard_raft.sh`)
- **Shards**: 2
- **Replicas per shard**: 3
- **Total processes**: 6
- **Threads**: 6
- **Duration**: Up to 120 s (polling for completion)

### 4.2 Execution Sequence

```
1. Kill lingering processes, clean logs
2. Start shard 0: 3 replicas via shard_raft.sh 2 0 6 {localhost,p2,p1} 0 1
3. Sleep 5s (prevent port conflicts between shards)
4. Start shard 1: 3 replicas via shard_raft.sh 2 1 6 {localhost,p2,p1} 0 1
5. Poll for completion (max 120s):
   - Check both shard0-localhost.log and shard1-localhost.log
   - for "agg_persist_throughput" keyword
   - 1s polling interval, 10s progress reports
6. Graceful shutdown: SIGTERM → 3s wait → SIGKILL
7. Parse logs
```

### 4.3 Shutdown Procedure

This test uses a more careful multi-phase shutdown:

```
1. pkill -TERM bash/shard_raft.sh    (stop wrapper scripts first)
2. pkill -TERM dbtest                 (graceful stop)
3. sleep 3
4. pkill -9 bash/shard_raft.sh       (force kill wrappers)
5. pkill -9 dbtest                    (force kill binaries)
6. killall -9 dbtest                  (fallback)
7. sleep 2
8. wait $SHARD0_PID $SHARD1_PID
```

### 4.4 Pass/Fail Criteria

For each shard (0 and 1):

| Criterion | Threshold | Log File | Grep Pattern |
|-----------|-----------|----------|--------------|
| Throughput reported | Present | `shard{i}-localhost.log` | `agg_persist_throughput` |
| Abort ratio | < 40% | `shard{i}-localhost.log` | `NewOrder_remote_abort_ratio:` |

The abort ratio threshold is 40% (vs 20% for single-shard) because
cross-shard transactions have higher contention.

Additionally, Raft replication metrics are checked for informational
purposes (warnings, not failures):

| Metric | Check | Log Files |
|--------|-------|-----------|
| `replay_batch` | Warning if < 1000 | `shard{i}-{localhost,p1,p2}.log` |

### 4.5 What It Tests

- Cross-shard TPC-C transactions with Raft replication
- Two independent Raft groups operating concurrently
- Port isolation between shard 0 and shard 1
- Completion polling (not fixed sleep) for robustness

## 5. Scenario 4: shard1ReplicationSimpleRaft

**Script**: `examples/mako-raft-tests/test_1shard_replication_simple_raft.sh` (149 lines)

**Purpose**: Runs simple key-value transactions on a single shard with
3 Raft replicas using the `simpleTransactionRepRaft` binary.  Tests data
integrity by verifying all replicas have identical committed state.

### 5.1 Configuration

- **Binary**: `simpleTransactionRepRaft`
- **Shards**: 1
- **Replicas**: 3 (localhost, p1, p2)
- **Threads**: 6
- **Duration**: 40 s

### 5.2 Execution Sequence

```
1. Kill lingering processes
2. Clean old logs and RocksDB data
3. Start 3 replicas:
   simpleTransactionRepRaft 1 0 6 {localhost,p2,p1} 1
4. Sleep 40s
5. Kill all PIDs → wait → pkill -9 → sleep 2
6. Parse logs for replay_batch and data integrity
```

### 5.3 Pass/Fail Criteria

| Criterion | Threshold | Log Files | Grep Pattern |
|-----------|-----------|-----------|--------------|
| Follower replication | > 0 batches | p1 log | `replay_batch:` |
| p1 data integrity | Pass | p1 log | `ALL VERIFICATIONS PASSED` |
| p2 data integrity | Pass | p2 log | `ALL VERIFICATIONS PASSED` |
| Both followers verified | >= 2 | — | — |

The leader's verification is not required for pass (leader may hang
during shutdown — documented as a known issue).  If the leader didn't
print `ALL VERIFICATIONS PASSED`, a warning is shown instead of failure.

### 5.4 What It Tests

- End-to-end data integrity: write on leader, replicate via Raft,
  verify identical state on followers
- `simpleTransactionRepRaft` binary's built-in verification (compares
  committed values across replicas)
- Raft replication of simple key-value workload

## 6. Scenario 5: shard2ReplicationSimpleRaft

**Script**: `examples/mako-raft-tests/test_2shard_replication_simple_raft.sh` (171 lines)

**Purpose**: Runs simple key-value transactions on 2 shards, each with
3 Raft replicas (6 total processes).  Tests multi-shard data integrity.

### 6.1 Configuration

- **Binary**: `simpleTransactionRepRaft`
- **Shards**: 2
- **Replicas per shard**: 3
- **Total processes**: 6
- **Threads**: 6
- **Duration**: 60 s

### 6.2 Execution Sequence

```
1. Kill lingering processes, clean logs
2. Start shard 0: simpleTransactionRepRaft 2 0 6 {localhost,p2,p1} 1
3. Sleep 2s
4. Start shard 1: simpleTransactionRepRaft 2 1 6 {localhost,p2,p1} 1
5. Sleep 60s
6. Kill all 6 PIDs → wait → pkill -9 → sleep 2
7. Parse logs
```

### 6.3 Pass/Fail Criteria

For each shard (0 and 1):

| Criterion | Threshold | Log Files | Grep Pattern |
|-----------|-----------|-----------|--------------|
| Follower replication | > 0 batches | `simple-raft-shard{i}-p1.log` | `replay_batch:` |

Data integrity check across all 6 logs:

| Criterion | Threshold | Grep Pattern |
|-----------|-----------|--------------|
| All 4 followers verified | >= 4 | `ALL VERIFICATIONS PASSED` |
| Leaders verified | Informational | `ALL VERIFICATIONS PASSED` |

Pass requires: `follower_verified >= 4` and `failed == 0`.

### 6.4 What It Tests

- Multi-shard data integrity under Raft replication
- Two independent Raft groups with separate state
- All 4 followers (2 per shard) have verified identical state

## 7. Process Naming and Binaries

| Binary | Purpose | Built From |
|--------|---------|-----------|
| `simpleRaft` | Standalone Raft replication test | `src/deptran/raft/` |
| `dbtest` | Full Mako transaction processor with TPC-C | `src/mako/` + `src/deptran/` |
| `simpleTransactionRepRaft` | Simple key-value with Raft replication | `src/mako/` |

## 8. Known Issues Handled in Scripts

### 8.1 Leader Shutdown Hang

All scripts handle the known issue where the Raft leader may hang during
shutdown.  Scripts use multi-phase kill (SIGTERM → sleep → SIGKILL) and
do not require leader-side verification for pass.

### 8.2 Port Conflicts

The 2-shard tests insert a 5s delay between starting shard 0 and shard 1
to prevent port conflicts during startup.  Port ranges are defined in
the config YAML files:

- Shard 0: `raft6_shardidx0.yml` (ports 27001-27201)
- Shard 1: `raft6_shardidx1.yml` (ports 27002-27202)

### 8.3 RocksDB Cleanup

Simple transaction tests clean `/tmp/${USER}_mako_rocksdb_shard*` before
each run to prevent stale persistent state from affecting results.

## 9. Raft vs Paxos Scenario Comparison

| Aspect | Raft Scenarios | Paxos Scenarios |
|--------|---------------|-----------------|
| Replicas per shard | 3 | 3 + 1 learner |
| Launch script | `shard_raft.sh` | `shard.sh` |
| Config files | `raft*_shardidx*.yml` + `occ_raft.yml` | `paxos*_shardidx*.yml` + `occ_paxos.yml` |
| Port range | 27xxx | 17xxx |
| `--replication` flag | `raft` | Not specified (default Paxos) |
| Simple binary | `simpleTransactionRepRaft` | `simpleTransactionRep` |
| Basic test binary | `simpleRaft` | `simplePaxos` |
| Learner callbacks | N/A | Checked separately |
| Leader shutdown hang | Known issue | Not reported |
