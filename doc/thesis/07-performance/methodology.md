# Benchmark Methodology

## 1. Overview

This chapter describes the methodology used to compare Raft and Paxos
replication performance in Mako.  All benchmarks use the CI test
infrastructure documented in the previous chapter, running on a single
localhost machine.

## 2. Test Environment

### 2.1 Hardware

All tests run on a single machine with all replicas co-located.  This
means network latency between replicas is effectively zero (localhost
loopback), so measured throughput reflects CPU overhead and
synchronisation costs rather than network round-trip time.

### 2.2 Transport

All tests use the **srpc (TCP/IP RPC)** transport backend (default).
eRPC (RDMA) is not used in these benchmarks.  The srpc transport adds
~10-50 us latency per RPC call even on localhost.

### 2.3 Build Configuration

- **Optimisation**: Release mode (`-O2`)
- **Concurrency control**: OCC (optimistic concurrency control)
- **Memory allocator**: jemalloc

## 3. Workloads

### 3.1 TPC-C Benchmark

The primary workload is TPC-C, the industry-standard OLTP benchmark.
TPC-C models a wholesale supplier with these transaction types:

| Transaction | Mix | Description |
|-------------|-----|-------------|
| NewOrder | 45% | Create a new order (read + write, cross-shard possible) |
| Payment | 43% | Process a payment (read + write) |
| Delivery | 4% | Deliver pending orders (batch write) |
| OrderStatus | 4% | Query order status (read-only) |
| StockLevel | 4% | Check stock levels (read-only) |

### 3.2 Simple Transaction Workload

A simple key-value workload using the `simpleTransactionRepRaft` /
`simpleTransactionRep` binaries.  Writes key-value pairs to the leader
and verifies all replicas have identical state after replication.

## 4. Test Configurations

### 4.1 1-Shard TPC-C

| Parameter | Paxos | Raft |
|-----------|-------|------|
| Shards | 1 | 1 |
| Replicas | 3 voters + 1 learner = 4 | 3 voters |
| Total processes | 4 | 3 |
| Worker threads | 6 | 6 |
| Warehouses per shard | 6 | 6 |
| Test duration | 40 s (Paxos CI) | 60 s (Raft CI) |
| Site config | `paxos6_shardidx0.yml` | `raft6_shardidx0.yml` |
| Mode config | `occ_paxos.yml` | `occ_raft.yml` |
| Port range | 17001-17301 | 27001-27201 |
| Shard config | `local-shards1-warehouses6.yml` | Same |
| Launch script | `bash/shard.sh` | `bash/shard_raft.sh` |

### 4.2 2-Shard TPC-C

| Parameter | Paxos | Raft |
|-----------|-------|------|
| Shards | 2 | 2 |
| Replicas per shard | 3 + 1 learner = 4 | 3 |
| Total processes | 8 | 6 |
| Worker threads | 6 per shard | 6 per shard |
| Warehouses per shard | 6 | 6 |
| Test duration | ~120 s (polling) | ~120 s (polling) |
| Cross-shard transactions | Yes (NewOrder, Payment) | Yes |
| Abort ratio threshold | < 40% | < 40% |

### 4.3 1-Shard Simple Transaction

| Parameter | Paxos | Raft |
|-----------|-------|------|
| Binary | `simpleTransactionRep` | `simpleTransactionRepRaft` |
| Replicas | 3 + 1 learner | 3 |
| Duration | 40 s | 40 s |
| Verification | `ALL VERIFICATIONS PASSED` | Same |

### 4.4 2-Shard Simple Transaction

| Parameter | Paxos | Raft |
|-----------|-------|------|
| Binary | `simpleTransactionRep` | `simpleTransactionRepRaft` |
| Replicas per shard | 3 + 1 learner | 3 |
| Duration | 60 s | 60 s |
| Total processes | 8 | 6 |

## 5. Metrics Collected

### 5.1 Primary Metrics

| Metric | Source | Description |
|--------|--------|-------------|
| `agg_persist_throughput` | Leader log | Aggregate persisted transaction throughput (ops/sec) |
| `replay_batch` | Follower log | Number of replication batches replayed by followers |
| `NewOrder_remote_abort_ratio` | Leader log | Percentage of NewOrder transactions aborted due to remote conflicts |

### 5.2 Per-Transaction Metrics

| Metric | Description |
|--------|-------------|
| `{TxType}_attempts` | Number of transaction attempts |
| `{TxType}_commits` | Number of successful commits |
| `{TxType}_avg_latency` | Average latency (microseconds) |
| `{TxType}_p50_latency` | Median latency |
| `{TxType}_p99_latency` | 99th percentile latency |
| `{TxType}_abort_ratio` | Abort rate (local + remote) |

### 5.3 Replication Metrics

| Metric | Description |
|--------|-------------|
| `follower_callbacks` | Number of committed entries received by follower |
| `leader_callbacks` | Number of committed entries on leader |
| Per-partition commit counts | Distribution of commits across partitions |

## 6. Measurement Procedure

### 6.1 TPC-C Tests

1. Clean environment (kill processes, remove stale data)
2. Start replicas (followers first, leader last)
3. Wait for leader election and stabilisation (~5 s)
4. Run benchmark for specified duration
5. Collect output from leader log
6. Collect replication metrics from follower logs
7. Kill all processes, archive logs

### 6.2 Simple Transaction Tests

1. Clean environment
2. Start all replicas
3. Run workload for specified duration
4. Wait for followers to finish replaying
5. Verify data integrity: `ALL VERIFICATIONS PASSED`
6. Collect `replay_batch` counts
7. Kill and archive

## 7. Caveats and Limitations

### 7.1 Single-Node Deployment

All replicas run on localhost.  This eliminates network latency as a
variable but introduces CPU contention between replicas.  In production,
replicas run on separate machines with dedicated CPU resources.

**Impact**: Throughput numbers are lower than production due to CPU
sharing.  The relative comparison (Raft vs Paxos) remains valid since
both share the same contention.

### 7.2 Test Duration Difference

The 1-shard Paxos test runs for 40 seconds while the Raft test runs
for 60 seconds.  Longer runs can accumulate more transactions but may
also encounter more steady-state effects.  The `agg_persist_throughput`
metric normalises to ops/sec to account for this.

### 7.3 Process Count Difference

Paxos uses 4 processes per shard (3 voters + 1 learner) while Raft uses
3 (all voters).  On a single machine, the extra Paxos learner process
competes for CPU resources, which may slightly reduce Paxos throughput
in single-node tests.

### 7.4 Replication Protocol Differences

| Aspect | Multi-Paxos | Raft |
|--------|-------------|------|
| Log ordering | Per-instance ordering | Strict sequential log |
| Commit condition | Majority of acceptors | Majority of voters |
| Leader election | External mechanism | Built-in (RequestVote) |
| Pipelining | Native (out-of-order) | Limited (sequential index) |
| Learner role | Separate non-voting replica | Not applicable |
| Batching | Batch size configurable | Batch size configurable |

Multi-Paxos can pipeline proposals across instances without waiting for
previous instances to commit.  Raft requires strict log ordering — each
entry must be committed in index order.  This pipelining advantage can
explain higher throughput in low-contention single-shard scenarios.

### 7.5 Warmup Period

Both Paxos and Raft tests include startup time for leader election and
RPC connection establishment (~5 seconds).  The `agg_persist_throughput`
metric is measured after the system reaches steady state.

### 7.6 Resource Contention

On a single machine with 6 threads per shard and 3-4 replicas, the
total active thread count is 18-24 for a 1-shard test and 36-48 for a
2-shard test.  CPU scheduling effects can cause variance between runs.
