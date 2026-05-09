# Shell Scripts Walkthrough

## 1. Overview

The `examples/mako-raft-tests/` directory contains shell scripts that
orchestrate Raft integration tests.  Five are invoked by the CI pipeline;
three additional scripts test preferred leader features independently.

| Script | Lines | CI-Invoked | Tests |
|--------|-------|------------|-------|
| `simpleRaft.sh` | 120 | Yes | Basic Raft replication (3 replicas, 300 logs) |
| `test_1shard_replication_raft.sh` | 153 | Yes | 1-shard TPC-C with Raft |
| `test_2shard_replication_raft.sh` | 208 | Yes | 2-shard TPC-C with Raft |
| `test_1shard_replication_simple_raft.sh` | 149 | Yes | 1-shard simple tx with Raft |
| `test_2shard_replication_simple_raft.sh` | 171 | Yes | 2-shard simple tx with Raft |
| `run_test1_preferred_startup.sh` | 361 | No | Preferred leader election (5-node) |
| `run_test_log_replication.sh` | 159 | No | Log replication to 5 replicas |
| `run_test_noops.sh` | 256 | No | NO-OPS watermark synchronisation |

Supporting script:

| Script | Lines | Purpose |
|--------|-------|---------|
| `bash/shard_raft.sh` | 39 | Launch a single Raft shard (dbtest) |

## 2. `simpleRaft.sh` — Basic Raft Replication

**Path**: `examples/mako-raft-tests/simpleRaft.sh`

### 2.1 Step-by-Step Walkthrough

```
Step 1:  Remove old logs (raft_a1.log, raft_a2.log, raft_a3.log)
Step 2:  killall simpleRaft, sleep 1
Step 3:  Start p1 (follower) → raft_a2.log  [background]
Step 4:  Sleep 2s (allow p1 to initialise)
Step 5:  Start p2 (follower) → raft_a3.log  [background]
Step 6:  Sleep 2s (allow p2 to initialise)
Step 7:  Start localhost (preferred leader) → raft_a1.log  [background]
Step 8:  Sleep 40s (wait for 300 logs × 5ms + election + shutdown)
Step 9:  tail -n 5 all three logs
Step 10: Parse follower_callbacks from raft_a2.log and raft_a3.log
Step 11: Parse leader_callbacks from raft_a1.log (informational)
Step 12: Assert both follower callbacks >= 300
Step 13: Kill all PIDs (SIGTERM → sleep 1 → SIGKILL → killall -9)
Step 14: Exit 0 (pass) or 1 (fail)
```

### 2.2 Key Design Choices

- **Followers start first**: Ensures they are ready to receive
  AppendEntries when the leader starts.
- **Fixed 40s sleep**: Matches the Paxos `simplePaxos.sh` duration.
- **Leader hang tolerated**: Leader may not print `RESULTS` due to
  shutdown timing, so only follower callbacks are required.

### 2.3 Log Parsing Functions

```bash
get_follower_callbacks() {
    grep "RESULTS.*follower_callbacks=" "$1" | \
      sed -E 's/.*follower_callbacks=([0-9]+).*/\1/' | tail -1
}

get_leader_callbacks() {
    grep "RESULTS.*leader_callbacks=" "$1" | \
      sed -E 's/.*leader_callbacks=([0-9]+).*/\1/' | tail -1
}
```

## 3. `test_1shard_replication_raft.sh` — 1-Shard TPC-C

**Path**: `examples/mako-raft-tests/test_1shard_replication_raft.sh`

### 3.1 Step-by-Step Walkthrough

```
Step 1:  Kill lingering dbtest/simpleRaft processes
Step 2:  Clean nfs_sync_* and /tmp/mako_rocksdb_shard*
Step 3:  Start 3 replicas via shard_raft.sh:
           shard_raft.sh 1 0 6 localhost 0 1  →  ..._shard0-localhost-6.log
           shard_raft.sh 1 0 6 p2 0 1         →  ..._shard0-p2-6.log
           sleep 1
           shard_raft.sh 1 0 6 p1 0 1         →  ..._shard0-p1-6.log
Step 4:  Sleep 60s (run TPC-C benchmark)
Step 5:  Kill SHARD0_PID, wait
Step 6:  Check leader log (localhost):
           - grep "agg_persist_throughput" → must exist
           - grep "NewOrder_remote_abort_ratio:" → extract value, assert < 20%
Step 7:  Check follower log (p1):
           - grep "replay_batch:" → extract last value, assert > 500
Step 8:  Exit 0 (all pass) or 1 (any fail)
```

### 3.2 Thread Count

The thread count is configurable via `$1` (default 6), which selects
both the warehouse configuration and the Raft site config:

```bash
trd=${1:-6}
# Resolves to: config/1leader_2followers/raft6_shardidx0.yml
```

## 4. `test_2shard_replication_raft.sh` — 2-Shard TPC-C

**Path**: `examples/mako-raft-tests/test_2shard_replication_raft.sh`

### 4.1 Step-by-Step Walkthrough

```
Step 1:  Clean logs, kill lingering processes
Step 2:  Start shard 0 (3 replicas):
           shard_raft.sh 2 0 6 localhost 0 1  →  shard0-localhost.log
           shard_raft.sh 2 0 6 p2 0 1         →  shard0-p2.log
           sleep 1
           shard_raft.sh 2 0 6 p1 0 1         →  shard0-p1.log
Step 3:  Sleep 5s (port isolation between shards)
Step 4:  Start shard 1 (3 replicas):
           shard_raft.sh 2 1 6 localhost 0 1  →  shard1-localhost.log
           sleep 1
           shard_raft.sh 2 1 6 p2 0 1         →  shard1-p2.log
           sleep 1
           shard_raft.sh 2 1 6 p1 0 1         →  shard1-p1.log
Step 5:  Completion polling loop (max 120s):
           Check shard0-localhost.log for "agg_persist_throughput"
           Check shard1-localhost.log for "agg_persist_throughput"
           Both present → break; else sleep 1, report every 10s
Step 6:  Multi-phase shutdown:
           pkill -TERM bash/shard_raft.sh  (wrapper scripts)
           pkill -TERM dbtest              (binaries)
           sleep 3
           pkill -9 bash/shard_raft.sh
           pkill -9 dbtest
           killall -9 dbtest
           sleep 2
           wait $SHARD0_PID $SHARD1_PID
Step 7:  For each shard (0, 1):
           - Assert "agg_persist_throughput" present
           - Assert NewOrder_remote_abort_ratio < 40%
           - Report replay_batch counts (warning if < 1000)
Step 8:  Exit 0 or 1
```

### 4.2 Key Design: Completion Polling

Unlike the 1-shard test (fixed 60s sleep), the 2-shard test uses
active polling with a 120s timeout.  This is more robust because
cross-shard transactions can have variable completion times.

```bash
while [ $wait_count -lt $max_wait ]; do
    if grep -q "agg_persist_throughput" "$log_file0" && \
       grep -q "agg_persist_throughput" "$log_file1"; then
        break
    fi
    sleep 1
done
```

### 4.3 Key Design: Multi-Phase Shutdown

Wrapper scripts are killed before binaries to prevent respawning:

1. `pkill -TERM bash/shard_raft.sh` — stop wrappers first
2. `pkill -TERM dbtest` — graceful stop
3. Wait 3s for cleanup
4. `pkill -9` — force kill everything

## 5. `test_1shard_replication_simple_raft.sh` — 1-Shard Simple Tx

**Path**: `examples/mako-raft-tests/test_1shard_replication_simple_raft.sh`

### 5.1 Step-by-Step Walkthrough

```
Step 1:  Kill lingering simpleTransactionRepRaft/dbtest
Step 2:  Clean logs, RocksDB data
Step 3:  Start 3 replicas:
           simpleTransactionRepRaft 1 0 6 localhost 1 → simple-raft-shard0-localhost.log
           simpleTransactionRepRaft 1 0 6 p2 1       → simple-raft-shard0-p2.log
           sleep 1
           simpleTransactionRepRaft 1 0 6 p1 1       → simple-raft-shard0-p1.log
Step 4:  Sleep 40s
Step 5:  Kill PIDs → wait → pkill -9 → sleep 2
Step 6:  Check p1 follower: replay_batch > 0
Step 7:  Data integrity check (3 logs):
           For each {localhost, p2, p1}:
             grep "ALL VERIFICATIONS PASSED"
             localhost failure → warning (leader hang)
             follower failure → test fail
Step 8:  Pass requires: follower_verified >= 2 && failed == 0
```

### 5.2 Key Design: Leader Hang Tolerance

The leader's verification result is logged but a missing verification
on the leader is treated as a warning, not a failure:

```bash
if [ "$log_suffix" = "localhost" ]; then
    echo "⚠ Leader may have hung during shutdown (known issue)"
else
    echo "✗ Data integrity verification FAILED"
    failed=1
fi
```

## 6. `test_2shard_replication_simple_raft.sh` — 2-Shard Simple Tx

**Path**: `examples/mako-raft-tests/test_2shard_replication_simple_raft.sh`

### 6.1 Step-by-Step Walkthrough

```
Step 1:  Kill lingering processes, clean logs
Step 2:  Start shard 0 (3 replicas):
           simpleTransactionRepRaft 2 0 6 {localhost,p2} 1
           sleep 1
           simpleTransactionRepRaft 2 0 6 p1 1
Step 3:  Sleep 2s
Step 4:  Start shard 1 (3 replicas):
           simpleTransactionRepRaft 2 1 6 {localhost,p2} 1
           sleep 1
           simpleTransactionRepRaft 2 1 6 p1 1
Step 5:  Sleep 60s
Step 6:  Kill all 6 PIDs → wait → pkill -9 → sleep 2
Step 7:  For each shard (0, 1): check replay_batch > 0 on p1
Step 8:  Data integrity: check all 6 logs for "ALL VERIFICATIONS PASSED"
Step 9:  Pass requires: follower_verified >= 4 && failed == 0
```

## 7. `bash/shard_raft.sh` — Raft Shard Launcher

**Path**: `bash/shard_raft.sh`

### 7.1 Step-by-Step Walkthrough

```
Step 1:  Parse arguments: nshard=$1, shard=$2, trd=$3, cluster=$4,
         is_micro=$5, is_replicated=$6
Step 2:  mkdir -p results
Step 3:  Construct command:
           ./build/dbtest \
             --num-threads $trd \
             --shard-index $shard \
             --shard-config $path/config/local-shards${nshard}-warehouses${trd}.yml \
             -F config/1leader_2followers/raft${trd}_shardidx${shard}.yml \
             -F config/occ_raft.yml \
             -P $cluster \
             --replication raft
Step 4:  Conditionally add --is-micro and --is-replicated flags
Step 5:  Print configuration summary
Step 6:  eval $CMD
```

### 7.2 Config File Selection

The script selects config files based on thread count and shard index:

| Argument | Config File |
|----------|------------|
| `trd=6, shard=0` | `raft6_shardidx0.yml` |
| `trd=6, shard=1` | `raft6_shardidx1.yml` |

The `--replication raft` flag ensures the dispatcher routes to
`raft_impl` namespace even if YAML auto-detection fails.

## 8. Non-CI Scripts

### 8.1 `run_test1_preferred_startup.sh` (361 lines)

**Purpose**: Verify TimeoutNow leadership transfer protocol.

- **Cluster**: 5 nodes (localhost = preferred, p1-p4 = followers)
- **Binary**: `testPreferredReplicaStartup`
- **Duration**: ~35 seconds
- **Process**:
  1. Start all 5 replicas
  2. Monitor for 35s with early-exit detection
  3. Wait for all processes
  4. Analyse logs: count "BECAME LEADER" events per node
  5. Pass: localhost became leader >= 1 time, p1-p4 never became leader,
     all processes exit 0

### 8.2 `run_test_log_replication.sh` (159 lines)

**Purpose**: Verify 25 logs replicated to all 5 replicas.

- **Binary**: `testPreferredReplicaLogReplication`
- **Cluster**: 5 nodes
- **Duration**: ~10-15 seconds
- **Process**:
  1. Launch all 5 replicas in parallel
  2. Wait for all to complete (poll with progress indicator)
  3. Extract metrics: Role, Logs applied, PASS/FAIL status
  4. Pass: all 5 replicas report PASS

### 8.3 `run_test_noops.sh` (256 lines)

**Purpose**: Verify NO-OPS watermark synchronisation with preferred
leader.

- **Binary**: `testNoOps`
- **Cluster**: 5 nodes
- **Duration**: ~25 seconds
- **Process**:
  1. Launch all 5 replicas
  2. Monitor with 2s polling, status updates every 10s
  3. Timeout at 25s → SIGKILL remaining
  4. Extract metrics: NO-OPS applied, regular logs applied, max epoch
  5. Pass: all 5 replicas report "OVERALL: ALL TESTS PASSED"

### 8.4 Common Patterns in Non-CI Scripts

All three non-CI scripts share a consistent pattern:

| Feature | Details |
|---------|---------|
| Colour output | RED, GREEN, YELLOW, BLUE terminal codes |
| Log directory | `logs/test_*` or `logs_noops_test` under script dir |
| Cleanup trap | `trap cleanup EXIT` for SIGKILL on exit |
| Progress monitoring | Polling loop with elapsed time display |
| Result collection | Parse per-replica log files for pass/fail markers |
| Exit code | 0 on pass, 1 on fail |

## 9. Script Dependency Graph

```
ci_mako_raft.sh (or ci.sh)
├── compile()
│     └── make -j32
├── run_simple_raft()
│     └── examples/mako-raft-tests/simpleRaft.sh
│           └── build/simpleRaft {localhost,p1,p2}
├── run_1shard_replication_raft()
│     └── examples/mako-raft-tests/test_1shard_replication_raft.sh
│           └── bash/shard_raft.sh 1 0 6 {localhost,p2,p1} 0 1
│                 └── build/dbtest --replication raft ...
├── run_2shard_replication_raft()
│     └── examples/mako-raft-tests/test_2shard_replication_raft.sh
│           ├── bash/shard_raft.sh 2 0 6 {localhost,p2,p1} 0 1
│           └── bash/shard_raft.sh 2 1 6 {localhost,p2,p1} 0 1
├── run_1shard_replication_simple_raft()
│     └── examples/mako-raft-tests/test_1shard_replication_simple_raft.sh
│           └── build/simpleTransactionRepRaft 1 0 6 {localhost,p2,p1} 1
└── run_2shard_replication_simple_raft()
      └── examples/mako-raft-tests/test_2shard_replication_simple_raft.sh
            ├── build/simpleTransactionRepRaft 2 0 6 {localhost,p2,p1} 1
            └── build/simpleTransactionRepRaft 2 1 6 {localhost,p2,p1} 1
```
