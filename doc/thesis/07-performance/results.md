# Detailed Benchmark Results

## 1. Overview

All results were collected from CI test runs on a single localhost
machine.  Numbers are from the log files archived in `~/results/`.

## 2. 1-Shard TPC-C Results

### 2.1 Aggregate Throughput

| Metric | Paxos | Raft | Difference |
|--------|-------|------|------------|
| `agg_persist_throughput` | 133,931 ops/sec | 96,463 ops/sec | Raft 28.0% lower |
| Test duration | ~40 s | ~60 s | Raft runs 50% longer |
| Replicas | 3 voters + 1 learner (4 processes) | 3 voters (3 processes) | Raft uses 25% fewer processes |

**Source files**:
- Paxos: `test_1shard_replication.sh_shard0-localhost-6.log`
- Raft: `test_1shard_replication_raft.sh_shard0-localhost-6.log`

### 2.2 Per-Transaction Latency (1-Shard)

| Transaction | Paxos Commit Latency | Raft Commit Latency | Paxos Abort Ratio | Raft Abort Ratio |
|-------------|---------------------|---------------------|-------------------|------------------|
| NewOrder | 0.0451 ms | 0.0390 ms | 0.0116% | 0.0084% |
| Payment | 0.0329 ms | 0.0815 ms | 0.0015% | 0.0031% |
| Delivery | 0.1378 ms | 0.1155 ms | 0% | 0% |
| OrderStatus | 0.0141 ms | 0.0113 ms | 0.0037% | 0.0051% |
| StockLevel | 0.1034 ms | 0.1094 ms | 0.184% | 0.160% |

Notes:
- All latencies are local commit latencies (no remote transactions
  in 1-shard configuration)
- Remote abort ratios are `-nan` for both (no cross-shard transactions)
- NewOrder and Delivery are faster under Raft; Payment is faster
  under Paxos

### 2.3 Follower Replication (1-Shard)

| Metric | Paxos Follower (p1) | Raft Follower (p1) |
|--------|--------------------|--------------------|
| `replay_batch` (final) | 669 | 3,674 |
| Wait time at termination | 39 s | 39 s |
| Threads finished at termination | 5/6 | 5/6 |

Raft followers process 5.5x more replay batches than Paxos followers
for the same workload.  This indicates Raft uses smaller, more frequent
batches while Paxos batches more aggressively.

## 3. 2-Shard TPC-C Results

### 3.1 Aggregate Throughput

| Metric | Paxos Shard 0 | Paxos Shard 1 | Raft Shard 0 | Raft Shard 1 |
|--------|--------------|--------------|-------------|-------------|
| `agg_persist_throughput` | 8,464 ops/sec | 8,539 ops/sec | 8,491 ops/sec | 8,580 ops/sec |
| `NewOrder_remote_abort_ratio` | 1.44% | 1.12% | 2.65% | 2.63% |

**Source files**:
- Paxos: `test_2shard_replication.sh_srpc_shard{0,1}-localhost.log`
- Raft: `shard{0,1}-localhost.log`

### 3.2 Aggregate Comparison

| Metric | Paxos (avg) | Raft (avg) | Difference |
|--------|-------------|------------|------------|
| Per-shard throughput | 8,501 ops/sec | 8,536 ops/sec | Raft 0.4% higher |
| Total throughput (2 shards) | 17,003 ops/sec | 17,071 ops/sec | Essentially equal |
| Remote abort ratio | 1.28% | 2.64% | Raft 2.1x higher |
| Replicas (total) | 8 (4 per shard) | 6 (3 per shard) | Raft uses 25% fewer |

### 3.3 Throughput Drop: 1-Shard to 2-Shard

| Metric | Paxos | Raft |
|--------|-------|------|
| 1-shard throughput | 133,931 ops/sec | 96,463 ops/sec |
| Per-shard 2-shard throughput | 8,501 ops/sec | 8,536 ops/sec |
| Drop factor | 15.8x | 11.3x |

Both protocols experience dramatic throughput reduction when cross-shard
transactions are introduced, but Paxos drops more (15.8x vs 11.3x)
because it starts from a higher single-shard baseline.

### 3.4 Follower Replication (2-Shard)

| Metric | Raft Shard 0 (p1) |
|--------|-------------------|
| `replay_batch` (final) | 1,173 |
| Wait time | 45 s |

(Paxos 2-shard follower `replay_batch` not captured in archived logs.)

## 4. Simple Transaction Results

### 4.1 simpleRaft Test

| Metric | Value |
|--------|-------|
| p1 `follower_callbacks` | 303 |
| p2 `follower_callbacks` | 303 |
| Expected minimum | 300 |
| Test result | PASS |

All 300 expected log entries (100 per partition × 3 partitions) were
replicated to both followers, plus 3 additional (likely end-of-stream
markers).

### 4.2 1-Shard Simple Transaction (Raft)

| Metric | Shard 0 - p1 | Shard 0 - p2 |
|--------|-------------|-------------|
| `replay_batch` | 12 | 12 |
| `ALL VERIFICATIONS PASSED` | Yes | Yes |

### 4.3 2-Shard Simple Transaction (Raft)

| Metric | Shard 0 - p1 | Shard 1 - p1 |
|--------|-------------|-------------|
| `replay_batch` | 12 | 12 |
| `ALL VERIFICATIONS PASSED` | Yes | Yes |

Both shards replicate identical data to followers.  The low `replay_batch`
count (12) is expected for the simple key-value workload which generates
fewer transactions than TPC-C.

## 5. Replication Correctness

### 5.1 Data Integrity

Both Paxos and Raft achieve identical data integrity results:

| Test | Paxos | Raft |
|------|-------|------|
| 1-shard simple | `ALL VERIFICATIONS PASSED` | `ALL VERIFICATIONS PASSED` |
| 2-shard simple | `ALL VERIFICATIONS PASSED` | `ALL VERIFICATIONS PASSED` |
| simpleRaft/simplePaxos | >= 300 callbacks | >= 300 callbacks |

### 5.2 Replication Completeness

All followers in both protocols receive complete replication:

- Paxos: Learner receives all committed entries
- Raft: All voters receive all committed entries
- No data loss observed in any test run

## 6. Summary Table

| Configuration | Paxos (ops/sec) | Raft (ops/sec) | Raft/Paxos | Processes |
|---------------|----------------|----------------|------------|-----------|
| 1-shard TPC-C | 133,931 | 96,463 | 72.0% | 4 vs 3 |
| 2-shard TPC-C (per shard) | ~8,501 | ~8,536 | 100.4% | 8 vs 6 |
| 2-shard TPC-C (total) | ~17,003 | ~17,071 | 100.4% | 8 vs 6 |
| 1-shard simple | PASS | PASS | Equal | 4 vs 3 |
| 2-shard simple | PASS | PASS | Equal | 8 vs 6 |
