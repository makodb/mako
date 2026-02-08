# Performance Analysis and Discussion

## 1. Overview

This chapter analyses the benchmark results from the previous chapter,
explaining why Paxos and Raft perform differently under various
configurations and what the results mean for production deployment.

## 2. Single-Shard Throughput Gap

### 2.1 The Observation

In the 1-shard TPC-C benchmark, Paxos achieves 133,931 ops/sec compared
to Raft's 96,463 ops/sec — a 28% throughput advantage for Paxos.  Three
factors contribute to this gap.

### 2.2 Factor 1: Multi-Paxos Pipelining

Multi-Paxos can pipeline proposals across independent instances without
waiting for previous instances to commit.  Each log slot is an
independent Paxos instance, so the leader can propose entries for slots
N, N+1, N+2 concurrently.

Raft enforces strict sequential log ordering.  Each entry must be
committed at a specific log index, and entries are committed in order.
The leader cannot advance the commit index past a gap.  This means a
slow follower can delay the commit of subsequent entries.

In a single-shard test with no cross-shard coordination, the replication
layer is the primary bottleneck.  Paxos's ability to pipeline gives it
a throughput advantage because the leader can keep its proposal pipeline
full without waiting for sequential commits.

### 2.3 Factor 2: Test Duration Difference

The Paxos 1-shard test runs for 40 seconds while the Raft test runs
for 60 seconds.  Both tests include a startup phase (~5 seconds) for
leader election and RPC connection establishment.

In a 40-second test, the startup overhead represents ~12.5% of the
total runtime.  In a 60-second test, it represents ~8.3%.  However,
the `agg_persist_throughput` metric is calculated as total committed
transactions divided by measured runtime, so it normalises for duration.

The longer Raft test may encounter more steady-state effects (memory
pressure, GC, log growth) that reduce average throughput.  This is a
minor factor compared to the pipelining difference, but it may account
for 2-5% of the gap.

### 2.4 Factor 3: Process Count and CPU Contention

Paxos uses 4 processes per shard (3 voters + 1 learner) while Raft uses
3 (all voters).  On a single machine, more processes means more CPU
contention.

Paradoxically, the extra Paxos learner process should *reduce* Paxos
throughput (more CPU contention) rather than increase it.  Yet Paxos is
faster.  This means the pipelining advantage more than compensates for
the additional CPU overhead.

The extra learner in Paxos does not participate in the quorum, so it
does not add latency to the commit path.  It receives committed entries
asynchronously, similar to Raft followers.

### 2.5 Per-Transaction Latency Analysis

The per-transaction latencies show an interesting mixed picture:

| Transaction | Faster Protocol | Difference |
|-------------|-----------------|------------|
| NewOrder | Raft (0.039 ms vs 0.045 ms) | 13% faster |
| Payment | Paxos (0.033 ms vs 0.082 ms) | 60% faster |
| Delivery | Raft (0.116 ms vs 0.138 ms) | 16% faster |
| OrderStatus | Raft (0.011 ms vs 0.014 ms) | 19% faster |
| StockLevel | Paxos (0.103 ms vs 0.109 ms) | 6% faster |

Raft has lower latency for 3 of 5 transaction types (NewOrder, Delivery,
OrderStatus) but higher latency for Payment, which constitutes 43% of
the TPC-C mix.  Payment is a simple read-write transaction; its higher
latency under Raft may be related to how Raft batches Payment operations
differently from Paxos.

Despite lower per-transaction latency for most types, Raft's aggregate
throughput is lower because:
1. The sequential commit requirement limits concurrency at the
   replication layer.
2. Payment (43% of transactions) is significantly slower under Raft.
3. Aggregate throughput depends on commit pipeline depth, not just
   individual transaction latency.

## 3. Two-Shard Throughput Convergence

### 3.1 The Observation

In the 2-shard TPC-C benchmark, per-shard throughput is nearly identical:
Paxos ~8,501 ops/sec vs Raft ~8,536 ops/sec (0.4% difference, within
measurement noise).

### 3.2 Why Cross-Shard Coordination Dominates

When transactions span two shards, the coordination protocol (2PC or
similar) adds ~10 ms of cross-shard round-trip latency per transaction.
NewOrder transactions have a ~5.4% cross-shard ratio, and Payment
transactions have an ~8.2% cross-shard ratio.

The cross-shard commit latency (~10 ms) is 200x larger than the
intra-shard Raft commit latency (~0.05 ms) and the intra-shard Paxos
commit latency (~0.04 ms).  At this scale, the difference between Raft
and Paxos replication latency is negligible compared to the cross-shard
coordination overhead.

The bottleneck shifts from the replication layer to the coordination
layer.  Both protocols perform essentially the same because the
replication protocol is no longer the limiting factor.

### 3.3 Abort Ratio Increase

The 2-shard configuration shows higher abort ratios than 1-shard:

| Metric | 1-Shard (Paxos) | 2-Shard (Paxos) | 2-Shard (Raft) |
|--------|-----------------|-----------------|-----------------|
| NewOrder local abort ratio | 0.012% | 0.31-0.91% | 0.97-1.95% |
| NewOrder remote abort ratio | N/A | 0.90-1.44% | 1.79-2.71% |
| StockLevel abort ratio | 0.18% | 1.04-1.38% | 1.45-1.53% |

Cross-shard transactions contend for locks across shards, increasing
abort rates.  Raft shows higher abort ratios (roughly 2x Paxos) in the
2-shard configuration, which may be due to subtle timing differences in
how Raft's sequential commit interacts with cross-shard lock acquisition.

### 3.4 Throughput Drop Factor

Both protocols experience dramatic throughput reduction from 1-shard to
2-shard operation:

- Paxos: 133,931 → 8,501 per shard (15.8x drop)
- Raft: 96,463 → 8,536 per shard (11.3x drop)

Paxos drops more (15.8x vs 11.3x) because it starts from a higher
single-shard baseline.  Both converge to approximately the same 2-shard
throughput (~8,500 ops/sec per shard), confirming that the cross-shard
coordination overhead is the dominant factor and is protocol-independent.

## 4. Replication Batching Behaviour

### 4.1 The Observation

In the 1-shard configuration, Raft followers process 3,674 replay
batches compared to Paxos's 669 — a 5.5x difference.

### 4.2 What This Means

The `replay_batch` metric counts the number of times the follower's
replay loop processes a batch of committed entries.  A higher batch
count with the same total throughput means smaller average batch sizes.

| Protocol | replay_batch | Total Commits | Avg Entries/Batch |
|----------|-------------|---------------|-------------------|
| Paxos | 669 | ~4,052,553 | ~6,058 |
| Raft | 3,674 | ~2,915,817 | ~794 |

Paxos batches ~7.6x more entries per batch.  This suggests Multi-Paxos
aggregates more entries before followers process them, which is
consistent with the pipelining design: the leader proposes many entries
concurrently, and followers receive and process them in large batches.

Raft's sequential commit means followers process entries more
incrementally — each committed index triggers a follower replay.  The
smaller batch size has two implications:

1. **Lower latency per entry**: Followers see each committed entry
   sooner because they do not wait for a large batch to accumulate.
2. **Higher per-batch overhead**: More batches means more overhead from
   batch processing bookkeeping, memory allocation, and synchronisation.

### 4.3 In the 2-Shard Configuration

The 2-shard Raft follower processes 1,165 replay batches (shard 0) and
1,032 replay batches (shard 1).  With ~259,000 total commits per shard,
this gives ~222-250 entries per batch — still smaller than Paxos but
closer because the overall throughput is lower.

## 5. Replica Topology Trade-offs

### 5.1 Process Count

| Configuration | Paxos | Raft | Reduction |
|---------------|-------|------|-----------|
| 1-shard | 4 (3 voters + 1 learner) | 3 (all voters) | 25% fewer |
| 2-shard | 8 (4 per shard) | 6 (3 per shard) | 25% fewer |

Raft uses 25% fewer processes because it does not require a learner
replica.  In production with dedicated machines, this means 25% fewer
servers for the same fault tolerance (2f+1 quorum).

### 5.2 Quorum Mechanics

Both protocols use majority quorum for commits:

| Protocol | Voters | Quorum Size | Fault Tolerance |
|----------|--------|-------------|-----------------|
| Paxos | 3 | 2 | 1 failure |
| Raft | 3 | 2 | 1 failure |

The fault tolerance is identical.  The Paxos learner is a non-voting
replica that receives committed entries for read scaling or backup.
Raft could achieve the same with a non-voting learner configuration
(not implemented in this version).

### 5.3 Leader Election

Paxos in this implementation uses an external leader election mechanism,
while Raft has built-in leader election via the RequestVote RPC.  Raft's
preferred leader mechanism (TimeoutNow) provides deterministic leader
placement, which is important for geo-replicated deployments.

## 6. Correctness and Data Integrity

### 6.1 Both Protocols Achieve Identical Correctness

All simple transaction tests produce `ALL VERIFICATIONS PASSED` for
both Paxos and Raft, across all follower replicas in both 1-shard and
2-shard configurations.  This confirms that:

1. The Raft implementation correctly replicates all committed
   transactions to followers.
2. Followers apply transactions in the same order as the leader.
3. Final state on all replicas is identical.

### 6.2 Replication Completeness

The simpleRaft test verifies that all 300 expected log entries (100 per
partition x 3 partitions) are replicated to both followers (303
callbacks, including end-of-stream markers).  This matches the
equivalent simplePaxos test behaviour.

## 7. Production Deployment Implications

### 7.1 When Paxos Is Better

Paxos's throughput advantage appears primarily in single-shard
(no cross-shard transaction) deployments where:

- The replication layer is the bottleneck.
- Workloads benefit from pipelined commits.
- Maximum single-shard throughput is the priority.

### 7.2 When Raft Offers Advantages

Raft may be preferred when:

- **Resource efficiency matters**: 25% fewer processes/servers for
  the same fault tolerance.
- **Multi-shard workloads dominate**: Cross-shard coordination
  eliminates the Paxos pipelining advantage.  At ~8,500 ops/sec per
  shard, both protocols are equivalent.
- **Built-in leader election is desired**: Raft's RequestVote and
  TimeoutNow provide deterministic leader placement without external
  mechanisms.
- **Operational simplicity is valued**: Raft's single-leader model
  with sequential logs is easier to reason about, debug, and monitor.

### 7.3 Throughput in Context

The 1-shard throughput difference (133,931 vs 96,463 ops/sec) is
measured on localhost where network latency is zero.  In a production
geo-replicated deployment:

- Network round-trip time between replicas would be 1-100 ms depending
  on geography.
- This latency would dominate the commit path for both protocols.
- The effective throughput difference between Paxos and Raft would be
  smaller than observed in localhost benchmarks.
- The 2-shard results (where throughput converges) are more
  representative of production behaviour where coordination latency
  dominates.

### 7.4 Summary of Trade-offs

| Factor | Paxos | Raft |
|--------|-------|------|
| Single-shard throughput | Higher (133,931 ops/sec) | Lower (96,463 ops/sec) |
| Multi-shard throughput | ~8,500 ops/sec/shard | ~8,500 ops/sec/shard |
| Process overhead | 33% more (learner) | Baseline |
| Leader election | External | Built-in |
| Log ordering | Per-instance (pipelined) | Sequential |
| Follower replay latency | Higher (large batches) | Lower (smaller batches) |
| Correctness | Verified | Verified |
| Operational complexity | Higher | Lower |
