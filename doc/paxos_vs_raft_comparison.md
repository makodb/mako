# Mako Replication Layer Comparison: Paxos vs Raft

**Date:** 2026-02-07
**Branch:** mako-krish-new
**Machine:** Single-node localhost (all replicas co-located)

---

## 1. Executive Summary

This document presents a head-to-head comparison of Mako's two replication backends — **Multi-Paxos** and **Raft** — across four CI test scenarios. Both protocols are used as the atomic broadcast layer beneath Mako's OCC-based concurrency control.

### Key Finding

| Test | Paxos (ops/sec) | Raft (ops/sec) | Difference |
|------|---------------:|---------------:|------------|
| **1-Shard TPC-C** | **133,931** | 96,463 | Paxos +38.9% |
| **2-Shard TPC-C (Shard 0)** | 8,464 | 8,529 | ~Equal |
| **2-Shard TPC-C (Shard 1)** | 8,539 | 8,593 | ~Equal |

> **Single-shard throughput favors Paxos by ~39%.** Cross-shard (2-shard) throughput is virtually identical, as the bottleneck shifts to cross-shard coordination latency rather than the replication layer.

---

## 2. Test Environment

| Parameter | Value |
|-----------|-------|
| Host | Single localhost machine (all replicas on 127.0.0.1) |
| Transport | srpc (TCP/IP RPC) |
| Build | Release (`build/dbtest`, `build/simpleTransactionRep`, `build/simpleTransactionRepRaft`) |
| Worker threads per replica | 6 |
| Warehouses | 6 (1-shard) / 12 total (2-shard, 6 per shard) |
| Concurrency control | OCC (Optimistic Concurrency Control) |

---

## 3. Architectural Differences

### 3.1 Replica Topology

| Aspect | Paxos | Raft |
|--------|-------|------|
| **Replicas per shard** | 4 (leader + 2 followers + 1 learner) | 3 (leader + 2 followers) |
| **Total processes (1-shard)** | 4 | 3 |
| **Total processes (2-shard)** | 8 | 6 |
| **Learner nodes** | Yes (read-only catch-up replica) | No (not supported) |
| **Roles** | localhost (leader), p1, p2 (followers), learner | localhost (leader), p1, p2 (followers) |

**Impact:** Paxos runs 33% more processes per shard. The learner node in Paxos receives replicated data but does not participate in quorum voting, adding read scalability at the cost of extra network/CPU overhead.

### 3.2 Configuration Differences

**Atomic Broadcast Config:**
```yaml
# occ_paxos.yml              # occ_raft.yml
mode:                         mode:
  cc: occ                       cc: occ
  ab: multi_paxos               ab: raft
  read_only: occ                read_only: occ
  batch: false                  batch: false
  retry: 20                     retry: 20
  ongoing: 1                    ongoing: 1
```

**Replication Group Config (per shard):**
- **Paxos:** 4 entries per partition group (3 voters + 1 learner), ports 17xxx
- **Raft:** 3 entries per partition group (all voters), ports 27xxx

### 3.3 Binary Differences

| Component | Paxos | Raft |
|-----------|-------|------|
| TPC-C benchmark | `build/dbtest` | `build/dbtest` (same binary, `--replication raft` flag) |
| Simple transaction | `build/simpleTransactionRep` | `build/simpleTransactionRepRaft` (separate binary) |
| Shard launcher | `bash/shard.sh` | `bash/shard_raft.sh` |
| Config flag | `--replication=paxos` | `--replication raft` |
| Replication config | `paxos6_shardidx{N}.yml` | `raft6_shardidx{N}.yml` |

### 3.4 Helper Script Differences (`shard.sh` vs `shard_raft.sh`)

| Feature | `shard.sh` (Paxos) | `shard_raft.sh` (Raft) |
|---------|-------------------|----------------------|
| GDB support | Yes (`$GDB_PREFIX`) | No (direct `eval`) |
| LD_LIBRARY_PATH setup | Yes (for libtxlog.so) | No |
| Replication type param | 7th argument, defaults to "paxos" | Hardcoded to "raft" |
| Config selection | Dynamic (supports both paxos/raft) | Hardcoded raft configs |

---

## 4. TPC-C Benchmark Results (Detailed)

### 4.1 Single-Shard Replication

| Metric | Paxos | Raft |
|--------|------:|-----:|
| **agg_persist_throughput** | **133,931 ops/sec** | **96,463 ops/sec** |
| Total commits (n_commits) | 4,052,553 | 2,915,817 |
| Average latency | 0.0442 ms | 0.0616 ms |
| NewOrder commit latency | 0.0543 ms | 0.0390 ms |
| Payment commit latency | 0.0329 ms | 0.0815 ms |
| Delivery commit latency | 0.138 ms | 0.116 ms |
| OrderStatus commit latency | 0.0141 ms | 0.0113 ms |
| StockLevel commit latency | 0.103 ms | 0.109 ms |
| NewOrder local abort ratio | 0.012% | 0.008% |
| StockLevel local abort ratio | 0.184% | 0.160% |
| Follower replay_batch | 669 | 3,674 |
| Test duration | ~40s (poll-based) | 60s (fixed) |

**Commits per partition (Paxos):**
| par_id | 0 | 1 | 2 | 3 | 4 | 5 |
|--------|---|---|---|---|---|---|
| n_commits | 687,253 | 669,096 | 658,874 | 611,329 | 694,430 | 731,571 |

**Commits per partition (Raft):**
| par_id | 0 | 1 | 2 | 3 | 4 | 5 |
|--------|---|---|---|---|---|---|
| n_commits | 504,308 | 482,800 | 525,006 | 440,611 | 451,573 | 511,519 |

**Observations:**
- Paxos achieves 38.9% higher throughput in the single-shard scenario.
- Raft has significantly higher follower replay batches (3,674 vs 669), suggesting Raft batches replication more aggressively but with larger individual batch overhead.
- Individual transaction latencies are mixed: Raft is faster for NewOrder and OrderStatus, but slower for Payment.
- The Paxos test runs for ~40s (poll-based completion), while Raft runs for a fixed 60s, which may account for some throughput differences due to warm-up effects.

### 4.2 Two-Shard Replication

#### Shard 0

| Metric | Paxos | Raft |
|--------|------:|-----:|
| **agg_persist_throughput** | **8,464 ops/sec** | **8,529 ops/sec** |
| Total commits | 256,227 | 257,847 |
| Average latency | 0.0493 ms | 0.0487 ms |
| NewOrder commit latency (local) | 0.0570 ms | 0.0563 ms |
| NewOrder commit latency (remote) | 10.087 ms | 10.016 ms |
| NewOrder remote abort ratio | 1.44% | 2.33% |
| Payment remote commit latency | 11.491 ms | 11.403 ms |
| Payment remote abort ratio | 0.098% | 0.097% |

#### Shard 1

| Metric | Paxos | Raft |
|--------|------:|-----:|
| **agg_persist_throughput** | **8,539 ops/sec** | **8,593 ops/sec** |
| Total commits | 258,001 | 260,138 |
| Average latency | 0.0483 ms | 0.0471 ms |
| NewOrder commit latency (local) | 0.0557 ms | 0.0540 ms |
| NewOrder commit latency (remote) | 10.126 ms | 10.062 ms |
| NewOrder remote abort ratio | 1.12% | 1.60% |
| Payment remote commit latency | 11.523 ms | 11.429 ms |

**Observations:**
- **Two-shard throughput is nearly identical** between Paxos and Raft (~8,500 ops/sec per shard).
- The bottleneck in 2-shard mode is cross-shard transaction coordination (~10 ms remote NewOrder latency), not the replication layer.
- Raft has slightly higher NewOrder remote abort ratios (2.33% vs 1.44% on shard 0), suggesting marginally more contention during cross-shard commits.
- Both protocols show ~16x throughput reduction when going from 1-shard to 2-shard, confirming that cross-shard overhead dominates.

---

## 5. Simple Transaction Replication Results

The Simple transaction tests use a separate binary (`simpleTransactionRep` / `simpleTransactionRepRaft`) and focus on **replication correctness** rather than throughput. They do **not** report `agg_persist_throughput`.

### 5.1 Single-Shard Simple

| Metric | Paxos | Raft |
|--------|-------|------|
| Binary | `simpleTransactionRep` | `simpleTransactionRepRaft` |
| Test duration | 40s | 40s |
| replay_batch (p1 follower) | 6 | 6 |
| Data integrity (localhost) | N/A | PASSED |
| Data integrity (learner) | PASSED | N/A (no learner) |
| Data integrity (p2) | PASSED | PASSED |
| Data integrity (p1) | PASSED | PASSED |
| **Result** | **ALL PASSED** | **ALL PASSED** |

### 5.2 Two-Shard Simple

| Metric | Paxos | Raft |
|--------|-------|------|
| Test duration | 90s | 60s |
| replay_batch shard0-p1 | 12 | 12 |
| replay_batch shard1-p1 | 12 | 12 |
| Follower verifications | 6/6 PASSED | 6/6 PASSED |
| **Result** | **ALL PASSED** | **ALL PASSED** |

**Key Differences:**
- Paxos uses a 90s test duration for 2-shard simple; Raft uses 60s.
- Paxos verifies 3 followers per shard (learner, p2, p1); Raft verifies 2 followers per shard (p2, p1) + leader.
- Both achieve identical replay_batch counts (6 for 1-shard, 12 for 2-shard), indicating equivalent replication throughput for simple workloads.

---

## 6. Test Methodology Differences

### 6.1 Completion Detection

| Aspect | Paxos (`ci.sh`) | Raft (`ci_mako_raft.sh`) |
|--------|-----------------|--------------------------|
| 1-shard TPC-C | **Poll-based**: Grep for `agg_persist_throughput` every 1s, max 120s | **Fixed duration**: Sleep 60s then kill |
| 2-shard TPC-C | **Poll-based**: Check both shards, max 120s | **Poll-based**: Check both shards, max 120s |
| 1-shard Simple | **Fixed**: 40s | **Fixed**: 40s |
| 2-shard Simple | **Fixed**: 90s | **Fixed**: 60s |

**Impact on results:** The poll-based approach in Paxos 1-shard means the test ends as soon as the benchmark completes (~40s observed), while Raft's fixed 60s duration means the benchmark runs longer. This could explain part of the throughput difference — a shorter run may capture peak throughput, while a longer run includes steady-state effects.

### 6.2 Process Cleanup

| Aspect | Paxos | Raft |
|--------|-------|------|
| Shutdown method | SIGTERM + 3s grace + SIGKILL | Direct kill + SIGKILL fallback |
| Port range checked | 7001-8006, 31000-31100 | 27001-27106, 31000-31100 |
| GDB support | Yes (can run under gdb batch mode) | No |

### 6.3 Pass/Fail Criteria

| Check | Paxos | Raft |
|-------|-------|------|
| Throughput keyword | Required | Required |
| NewOrder abort ratio (1-shard) | < 20% | < 20% |
| NewOrder abort ratio (2-shard) | < 40% | < 40% |
| replay_batch (1-shard TPC-C) | > 500 | > 500 |
| replay_batch (1-shard Simple) | > 0 | > 0 |
| Data integrity | Required (Simple only) | Required (Simple only) |

---

## 7. Throughput Comparison Summary

```
                        Paxos           Raft            Winner
                        ─────           ────            ──────
1-Shard TPC-C:          133,931         96,463          Paxos (+38.9%)
2-Shard TPC-C (S0):       8,464          8,529          ~Tie  (+0.8% Raft)
2-Shard TPC-C (S1):       8,539          8,593          ~Tie  (+0.6% Raft)
2-Shard TPC-C (Total):   17,003         17,122          ~Tie  (+0.7% Raft)
```

### Throughput Drop: 1-Shard to 2-Shard

| Protocol | 1-Shard | 2-Shard (per shard) | Drop Factor |
|----------|--------:|--------------------:|:-----------:|
| Paxos | 133,931 | ~8,500 | ~15.8x |
| Raft | 96,463 | ~8,560 | ~11.3x |

The throughput drop from 1-shard to 2-shard is more severe for Paxos because it starts from a higher baseline. Both converge to ~8,500 ops/sec per shard when cross-shard coordination becomes the bottleneck.

---

## 8. Analysis and Discussion

### Why Paxos is faster in single-shard mode

1. **Test duration difference**: Paxos runs for ~40s (poll-based), Raft runs for a fixed 60s. The shorter Paxos run may capture a higher average during the most active period.

2. **Fewer replicas**: Despite having a learner node, Paxos only requires 2 out of 3 voters for quorum (the learner doesn't vote). Raft requires 2 out of 3 voters. The quorum size is equivalent, but Paxos's multi-decree protocol can pipeline proposals more aggressively.

3. **Multi-Paxos pipelining**: Multi-Paxos allows leaders to propose multiple values concurrently without waiting for each to commit. Raft's log-sequential nature requires each entry to be committed in order, which can be a throughput bottleneck under high load.

4. **Follower replay patterns**: Raft shows 3,674 replay batches vs Paxos's 669, suggesting Raft processes replication in many more smaller batches, adding per-batch overhead.

### Why 2-shard throughput is equal

In the 2-shard scenario, remote NewOrder transactions take ~10 ms for cross-shard coordination. This 10 ms cross-shard latency (vs sub-millisecond local operations) dominates total transaction time, making the replication layer's efficiency irrelevant to throughput. Both protocols converge to the same cross-shard coordination bottleneck.

### Replication correctness

Both protocols demonstrate identical replication correctness:
- All data integrity verifications pass on all followers
- Equivalent replay_batch counts on simple transaction tests
- No data loss or inconsistency detected in any test

---

## 9. Caveats

1. **Single-node testing**: All replicas run on localhost, eliminating network latency. Real-world geo-replicated deployments would show different relative performance.
2. **Single run**: These results are from a single run per test. Production benchmarking should use multiple runs with statistical analysis.
3. **Resource contention**: Running 4 (Paxos) vs 3 (Raft) processes on the same machine introduces different CPU/memory contention profiles.
4. **Test duration mismatch**: The 1-shard Paxos test uses poll-based completion (~40s) while Raft uses a fixed 60s timer. Normalizing test duration could change the comparison.
5. **Paxos 2-shard flakiness**: The initial Paxos shard2Replication run failed (shard 0 timed out after 120s with a segfault during shutdown). The retry succeeded. This suggests potential stability concerns under the Paxos 2-shard configuration.

---

## 10. Raw Test Output

### Paxos shard1Replication
```
agg_persist_throughput: 133,931 ops/sec
n_commits: 4,052,553
avg_latency: 0.0442 ms
replay_batch: 669
Result: PASSED
```

### Paxos shard2Replication
```
Shard 0: agg_persist_throughput: 8,464 ops/sec, n_commits: 256,227
Shard 1: agg_persist_throughput: 8,539 ops/sec, n_commits: 258,001
NewOrder_remote_abort_ratio: 1.44% / 1.12%
Result: PASSED (on retry; first run shard 0 timed out)
```

### Paxos shard1ReplicationSimple
```
replay_batch: 6
Data integrity: ALL VERIFICATIONS PASSED (3/3 followers)
Result: PASSED
```

### Paxos shard2ReplicationSimple
```
Shard 0 replay_batch: 12, Shard 1 replay_batch: 12
Data integrity: ALL VERIFICATIONS PASSED (6/6 followers)
Result: PASSED
```

### Raft shard1ReplicationRaft
```
agg_persist_throughput: 96,463 ops/sec
n_commits: 2,915,817
avg_latency: 0.0616 ms
replay_batch: 3,674
Result: PASSED
```

### Raft shard2ReplicationRaft
```
Shard 0: agg_persist_throughput: 8,529 ops/sec, n_commits: 257,847
Shard 1: agg_persist_throughput: 8,593 ops/sec, n_commits: 260,138
NewOrder_remote_abort_ratio: 2.33% / 1.60%
Result: PASSED
```

### Raft shard1ReplicationSimpleRaft
```
replay_batch: 6
Data integrity: ALL VERIFICATIONS PASSED (3/3 nodes)
Result: PASSED
```

### Raft shard2ReplicationSimpleRaft
```
Shard 0 replay_batch: 12, Shard 1 replay_batch: 12
Data integrity: ALL VERIFICATIONS PASSED (6/6 nodes)
Result: PASSED
```
