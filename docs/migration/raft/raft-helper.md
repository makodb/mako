# Mako-Raft: Raft as a Replication Layer

This document provides comprehensive documentation for using Raft as the replication layer in the Mako distributed transaction system.

## Overview

Mako supports Raft as an alternative to Paxos for consensus and replication. Raft provides a more understandable consensus algorithm while maintaining strong consistency guarantees. The Raft implementation in Mako includes:

- **Leader election** with configurable election timeouts
- **Log replication** with batching optimization
- **Preferred replica system** for deterministic leadership via TimeoutNow protocol
- **NO-OPS support** for watermark synchronization

## Building Mako with Raft

### Build Targets

Mako supports three replication modes. `SINGLE_RAFT_INSTANCE` now sets the **default runtime** Raft-group mode (single vs multi). You can still switch defaults via CMake/Make targets.

```bash
# Single-instance Raft (1 Raft group shared by all partitions) — RECOMMENDED
make mako-raft-single -j32

# Multi-instance Raft (1 Raft group per partition, mirrors Paxos 1:1 model)
make mako-raft-multi -j32

# Default Raft build (alias for single-instance)
make mako-raft -j32

# Paxos (default, no flags needed)
make -j32

# Show all available targets
make help
```

### CMake Flags

| Target | CMake Flags |
|--------|------------|
| `make` (Paxos) | `-DMAKO_USE_RAFT=OFF` (default) |
| `make mako-raft-single` | `-DMAKO_USE_RAFT=ON -DSINGLE_RAFT_INSTANCE=ON` |
| `make mako-raft-multi` | `-DMAKO_USE_RAFT=ON -DSINGLE_RAFT_INSTANCE=OFF` |
| `make raft-test` | `-DMAKO_USE_RAFT=ON -DRAFT_TEST=ON` |

### Manual CMake Build

```bash
# Single-instance Raft
cmake -S . -B build -DMAKO_USE_RAFT=ON -DSINGLE_RAFT_INSTANCE=ON
cmake --build build --parallel 32

# Multi-instance Raft
cmake -S . -B build -DMAKO_USE_RAFT=ON -DSINGLE_RAFT_INSTANCE=OFF
cmake --build build --parallel 32
```

### Runtime Override (Recommended for experiments)

You can override the Raft-group mode at runtime (no rebuild required):

```bash
# CLI flag
./build/dbtest ... --raft-groups=single
./build/dbtest ... --raft-groups=multi

# Or environment variable
MAKO_RAFT_GROUP_MODE=single ./build/dbtest ...
MAKO_RAFT_GROUP_MODE=multi ./build/dbtest ...
```

## Running the CI Tests

The Raft tests are organized in a CI script that mirrors the Paxos CI workflow.

### Main CI Script

```bash
# Run all Raft tests
./ci/ci_mako_raft.sh all

# Or run individual tests:
./ci/ci_mako_raft.sh compile                   # Build Mako with Raft support
./ci/ci_mako_raft.sh simpleRaft                # Basic Raft replication test
./ci/ci_mako_raft.sh shard1ReplicationRaft     # 1-shard Raft test (dbtest)
./ci/ci_mako_raft.sh shard2ReplicationRaft     # 2-shard Raft test (dbtest)
./ci/ci_mako_raft.sh shard1ReplicationSimpleRaft  # 1-shard Raft test (simpleTransactionRepRaft)
./ci/ci_mako_raft.sh shard2ReplicationSimpleRaft  # 2-shard Raft test (simpleTransactionRepRaft)
./ci/ci_mako_raft.sh cleanup                   # Clean up processes and temp files
```

### GitHub Actions CI

The Raft tests are integrated into the GitHub Actions CI workflow (`.github/workflows/ci.yml`). They run after the Paxos tests:

```
Paxos Tests
├── Compile (Paxos)
├── simplePaxos, shard1Replication, shard2Replication, etc.

Raft Tests
├── Compile Mako with Raft support
├── simpleRaft
├── shard1ReplicationRaft
├── shard2ReplicationRaft
├── shard1ReplicationSimpleRaft
├── shard2ReplicationSimpleRaft

Non-replication Tests
└── rocksdbTests

Cleanup
```

### Running Individual Tests Manually

The test scripts are located in `examples/mako-raft-tests/`:

```bash
# Simple 3-node Raft replication test
bash ./examples/mako-raft-tests/simpleRaft.sh

# 1-shard with 3 Raft replicas (dbtest - TPC-C workload)
bash ./examples/mako-raft-tests/test_1shard_replication_raft.sh

# 2-shard with Raft replication (dbtest - TPC-C workload)
bash ./examples/mako-raft-tests/test_2shard_replication_raft.sh

# 1-shard with Raft replication (simpleTransactionRepRaft - data integrity tests)
bash ./examples/mako-raft-tests/test_1shard_replication_simple_raft.sh

# 2-shard with Raft replication (simpleTransactionRepRaft - data integrity tests)
bash ./examples/mako-raft-tests/test_2shard_replication_simple_raft.sh

# Preferred replica startup test (5 nodes)
bash ./examples/mako-raft-tests/run_test1_preferred_startup.sh

# Log replication test
bash ./examples/mako-raft-tests/run_test_log_replication.sh

# NO-OPS watermark synchronization test
bash ./examples/mako-raft-tests/run_test_noops.sh
```

## Test Descriptions

### 1. simpleRaft (`simpleRaft.sh`)

**Purpose**: Basic 3-node Raft replication test (matches simplePaxos configuration)

**Configuration**:
- 3 Raft replicas: `localhost` (preferred leader), `p1`, `p2`
- 3 partitions, 100 log entries per partition (300 total)
- 3KB log size, 5ms between submissions
- Tests leader election and basic log replication

**What it verifies**:
- Leader election completes successfully
- Log entries are replicated to all followers
- Follower callback counts >= 300 (message_count * num_workers)
- Replication success based on follower verification

**Expected output**: Both followers verify replication success. Leader may hang during shutdown (known issue).

### 2. test_1shard_replication_raft (`test_1shard_replication_raft.sh`)

**Purpose**: Single shard with 3-way Raft replication using dbtest (TPC-C workload)

**Configuration**:
- 1 shard with 3 replicas: `localhost`, `p1`, `p2`
- Runs TPC-C-like workload for 60 seconds

**What it verifies**:
- `agg_persist_throughput` metric is present
- `NewOrder_remote_abort_ratio` < 20%
- Follower `replay_batch` count > 1000 (indicates active replication)

### 3. test_2shard_replication_raft (`test_2shard_replication_raft.sh`)

**Purpose**: Two shards, each with 3-way Raft replication using dbtest

**Configuration**:
- 2 shards (shard 0 and shard 1)
- Each shard has 3 replicas
- Tests cross-shard transactions with replication

**What it verifies**:
- Both shards show `agg_persist_throughput`
- `NewOrder_remote_abort_ratio` < 40%
- Replication metrics for all nodes

### 4. test_1shard_replication_simple_raft (`test_1shard_replication_simple_raft.sh`)

**Purpose**: Single shard with 3-way Raft replication using simpleTransactionRepRaft (data integrity tests)

**Configuration**:
- 1 shard with 3 replicas: `localhost`, `p1`, `p2`
- Runs simple transaction tests (basic, contention, overlapping keys, read-write)
- 40 second test duration

**What it verifies**:
- `replay_batch` count > 0 (replication is active)
- "ALL VERIFICATIONS PASSED" in follower logs (data integrity)
- Both followers must verify successfully (leader may hang during shutdown)

### 5. test_2shard_replication_simple_raft (`test_2shard_replication_simple_raft.sh`)

**Purpose**: Two shards with 3-way Raft replication using simpleTransactionRepRaft

**Configuration**:
- 2 shards, each with 3 replicas
- Runs simple transaction tests with cross-shard operations
- 60 second test duration

**What it verifies**:
- `replay_batch` count > 0 for both shards
- "ALL VERIFICATIONS PASSED" in all 4 follower logs
- All followers must verify successfully (leaders may hang during shutdown)

### 6. Preferred Replica Startup Test (`run_test1_preferred_startup.sh`)

**Purpose**: Verify TimeoutNow leadership transfer protocol

**Configuration**:
- 5-node cluster: `localhost` (preferred), `p1`, `p2`, `p3`, `p4`
- `localhost` is designated as the preferred leader

**What it verifies**:
- `localhost` becomes leader via TimeoutNow transfer
- Non-preferred replicas transfer leadership correctly
- Leadership stability over 30-second monitoring period

### 7. Log Replication Test (`run_test_log_replication.sh`)

**Purpose**: Verify log replication with TpcCommitCommand wrapping

**Configuration**:
- 5-node cluster
- Leader submits 25 logs wrapped in TpcCommitCommand
- Tests batching behavior

**What it verifies**:
- All 25 logs are replicated to all 5 replicas
- Log application callbacks fire correctly
- Throughput and replication timing metrics

### 8. NO-OPS Watermark Test (`run_test_noops.sh`)

**Purpose**: Verify NO-OPS message propagation for watermark synchronization

**Configuration**:
- 5-node cluster
- 5 NO-OPS messages (epochs 0-4)
- 10 regular logs after NO-OPS

**What it verifies**:
- NO-OPS messages propagate to all replicas
- Epoch ordering is maintained
- Regular logs work after watermark sync

## Test Comparison: Paxos vs Raft

### simpleRaft vs simplePaxos

| Aspect | simplePaxos | simpleRaft |
|--------|-------------|------------|
| Nodes | 4 (localhost, p1, p2, learner) | 3 (localhost, p1, p2) |
| Partitions | 3 workers | 3 workers |
| Messages | 100 logs per partition | 100 logs per partition |
| Log size | 3KB base | 3KB base |
| Sleep between logs | 5ms | 5ms |
| Total test time | ~40 seconds | ~40 seconds |

### shard1ReplicationSimpleRaft vs shard1ReplicationSimple

| Aspect | Paxos (simpleTransactionRep) | Raft (simpleTransactionRepRaft) |
|--------|------------------------------|--------------------------------|
| Binary | `simpleTransactionRep` | `simpleTransactionRepRaft` |
| Config | `paxos<N>_shardidx<X>.yml` | `raft<N>_shardidx<X>.yml` |
| Replicas per shard | 4 (with learner) | 3 (no learner) |
| Log files (1-shard) | 4 logs | 3 logs |
| Log files (2-shard) | 8 logs | 6 logs |

## Single vs Multi-Instance Raft

### Multi-Instance (1 Raft group per partition)

```
Partition 0 → RaftWorker0 → RaftServer0  (port 27001)
Partition 1 → RaftWorker1 → RaftServer1  (port 27002)
...
Partition N → RaftWorkerN → RaftServerN  (port 27000+N)
```

- Each partition has its own Raft log, election, and heartbeat loop
- Follower threads scale O(N) with partition count (verified: 2 at 1t, 4 at 4t, 8 at 8t)
- Mirrors Paxos 1:1 model (mentor-confirmed correct architecture)
- **Limitation:** Crashes at 12+ partitions due to FD exhaustion (`epoll_create: Too many open files`)

### Single-Instance (1 shared Raft group)

```
Partition 0 ──→ ┐
Partition 1 ──→ ├→ [Queue] → RaftWorker0 → RaftServer0 (port 27001)
Partition N ──→ ┘
                    Stub RPC servers on ports 27002..27000+N
```

- All partitions share one Raft log, one election, one heartbeat loop
- Follower threads stay constant at 2 regardless of partition count — O(1) overhead
- Per-partition callbacks route log entries to the correct partition after commit
- Stub servers on extra ports satisfy remote replicas' connection expectations
- **Advantage:** No FD exhaustion ceiling, 30-48% less CPU than Paxos/Multi-Raft

See `docs/dev/single_vs_multi_raft_analysis.md` for detailed architectural comparison.

## Scalability Benchmarking

### Running a Scalability Sweep

The thesis benchmarking infrastructure supports automated scalability sweeps across all backends:

```bash
# Build for the target backend first (make clean between switches!)
make clean && make mako-raft-single -j32

# Run scalability sweep
bash run_scalability_sweep.sh \
  --backend raft-single \
  --threads "1 2 4 6 8 12 16" \
  --runs 3 \
  --batch-size 400
```

Supported `--backend` values: `paxos`, `raft-single`, `raft-multi`

### Cross-Backend Comparison

```bash
python3 scripts/process_scalability_results.py \
  --paxos results/benchmarks/paxos/scalability_latest/results.csv \
  --raft-multi results/benchmarks/raft-multi/scalability_latest/results.csv \
  --raft-single results/benchmarks/raft-single/scalability_latest/results.csv
```

Produces tables for throughput, scaling efficiency, CPU usage, and reproducibility (CV%).

### Results Location

```
results/benchmarks/{backend}/scalability_{timestamp}/
├── summary.txt       # Human-readable report
├── results.csv       # Machine-readable (threads, throughput, cpu, latency, ...)
└── logs/             # Per-run leader/follower/CPU logs
```

A `scalability_latest` symlink always points to the most recent run.

### Config Generation

Raft configs for all thread counts are generated by:

```bash
cd config/1leader_2followers && python3 raft_generator.py
```

This generates `raft{N}_shardidx{S}.yml` for N=1..32, S=0..9. Port ranges: Raft 27xxx-36xxx (vs Paxos 17xxx-26xxx).

## Architecture

### Key Components

| Component | Location | Description |
|-----------|----------|-------------|
| RaftServer | `src/deptran/raft/server.{h,cc}` | Core Raft state machine and leader election |
| RaftWorker | `src/deptran/raft/raft_worker.{h,cc}` | Worker thread management and RPC servers |
| RaftCommo | `src/deptran/raft/commo.{h,cc}` | Communication layer for Raft RPCs |
| Frame | `src/deptran/raft/frame.{h,cc}` | Protocol frame registration |

### Raft vs Paxos Differences

| Feature | Raft | Paxos |
|---------|------|-------|
| Leadership | Single leader with term | Proposer-based |
| Learner node | All replicas equal (no learner) | Separate learner role |
| Election timeout | Randomized 400-700ms | N/A |
| Heartbeat interval | 5000us (configurable) | N/A |
| Replicas per shard | 3 | 4 (including learner) |

### Configuration Files

Raft-specific configuration files in `config/`:

- `none_raft.yml` - Base Raft configuration
- `occ_raft.yml` - OCC with Raft replication
- `rule_raft.yml` - Rule-based with Raft
- `config/1leader_2followers/raft*_shardidx*.yml` - Per-shard Raft configs

### Preferred Replica System

The preferred replica system ensures deterministic leadership:

1. Standard Raft voting happens (any replica can win initial election)
2. Non-preferred leader monitors for preferred replica liveness
3. When preferred replica is alive and caught up:
   - Non-preferred leader sends `TimeoutNow` RPC
   - Preferred replica starts immediate election
   - Preferred replica wins and becomes leader

```cpp
// Set preferred leader (all replicas should call with same site_id)
void SetPreferredLeader(siteid_t site_id);

// Get election timeout (shorter for preferred replica)
uint64_t GetElectionTimeout();
```

## Known Issues and TODOs

### Leader Shutdown Hang Issue

**Status**: Fixed in `f89ba8a6` (`raft: fix poll thread shutdown ordering`)

**Problem**: The Raft leader process hangs during shutdown and never completes verification. Followers shut down cleanly and pass.

**Affected Tests**:
- `simpleRaft` - leader (`raft_a1.log` / `localhost`) hangs
- `shard1ReplicationSimpleRaft` - leader may hang
- `shard2ReplicationSimpleRaft` - leaders may hang

**Root Cause**: Race condition between poll thread drain and `Server::~Server()` destructor:
1. `signal_stop()` is called, poll thread starts draining
2. `delete rpc_server_` is called
3. `Server::~Server()` sends `CmdRemovePollable` commands
4. Poll thread exits before processing all remove commands
5. `sconns_ctr_` never reaches 0
6. Server destructor waits forever

**Resolution**: RaftWorker now destroys the RPC servers while poll threads are
still alive, then stops and joins the poll threads exactly once. This lets the
server destructor enqueue and drain its pollable-removal commands before the
poll threads exit. The simpleRaft shutdown test now verifies clean exit for all
nodes.

### TODO List

- [x] **Leader shutdown hang**: Fixed by ordering RPC-server destruction before poll-thread shutdown
- [x] **Graceful shutdown coordination**: Covered by the RaftWorker shutdown ordering fix
- [ ] **Election timeout tuning**: Consider making election timeouts configurable per deployment
- [ ] **Snapshot support**: Implement log compaction via snapshots for long-running deployments

## Debugging Tips

### Check Raft Logs

```bash
# View leader election events
grep -E "BECAME LEADER|LOST LEADERSHIP" raft_a1.log

# Check replication progress
grep "replay_batch:" shard0-p1.log | tail -5

# Monitor shutdown sequence
grep -E "(RAFT-WORKER-SHUTDOWN|POLL-SHUTDOWN)" raft_a1.log

# Check for PASS/FAIL
grep -E "^(PASS|FAIL)$" raft_*.log

# Check data integrity verification
grep "ALL VERIFICATIONS PASSED" simple-raft-shard*.log

# Check follower callback counts
grep "follower_callbacks=" raft_*.log
```

### Common Issues

1. **Test timeout**: Increase sleep duration in test scripts
2. **Port conflicts**: Run cleanup before tests: `./ci/ci_mako_raft.sh cleanup`
3. **Leader hang**: Check if leader hung during shutdown - test still passes if followers verified
4. **Leftover processes**: Kill stale processes: `pkill -9 -f "build/simpleRaft"` or `pkill -9 -f "build/dbtest"`

### Cleanup Commands

```bash
# Full cleanup
./ci/ci_mako_raft.sh cleanup

# Manual cleanup
pkill -9 -f "build/simpleRaft"
pkill -9 -f "build/simpleTransactionRepRaft"
pkill -9 -f "build/dbtest"
pkill -9 -f "build/testPreferredReplicaStartup"
```

## File Structure

```
examples/mako-raft-tests/
├── simpleRaft.cc                    # Basic Raft replication test
├── simpleRaft.sh                    # Shell wrapper for simpleRaft
├── simpleTransactionRepRaft.cc      # Data integrity test with Raft
├── test_1shard_replication_raft.sh  # 1-shard dbtest with Raft
├── test_2shard_replication_raft.sh  # 2-shard dbtest with Raft
├── test_1shard_replication_simple_raft.sh  # 1-shard simple test with Raft
├── test_2shard_replication_simple_raft.sh  # 2-shard simple test with Raft
├── testPreferredReplicaStartup.cc   # Preferred replica test
├── testPreferredReplicaLogReplication.cc  # Log replication test
├── testNoOps.cc                     # NO-OPS watermark test
└── run_test*.sh                     # Shell wrappers for tests

ci/
├── ci.sh                            # Paxos CI script
└── ci_mako_raft.sh                  # Raft CI script

config/
├── none_raft.yml                    # Base Raft config
├── occ_raft.yml                     # OCC with Raft
└── 1leader_2followers/raft*_shardidx*.yml  # Per-shard configs
```

## References

- Raft paper: "In Search of an Understandable Consensus Algorithm" (Diego Ongaro, John Ousterhout)
- [Raft Visualization](https://raft.github.io/)
- Paxos documentation: `doc/paxos.md`
- Leader shutdown hang investigation: `doc/leader_shutdown_hang.md`
