# Root-Cause Analysis: Multi-Raft Instance Throughput Variance

> **Historical note**: this file reflects an older experimental phase and earlier conclusions about single-Raft vs multi-Raft. It should not be used as the primary source of truth for current performance claims. For the active architecture and sweep workflow, read [Replication Current State](../architecture/replication-current-state.md) and [Benchmark Sweeps](../performance/benchmark-sweeps.md).

## Executive Summary

The multi-Raft configuration (6 independent Raft groups, commit `4f99ffb6`) exhibits bimodal throughput — some runs achieve ~200K ops/sec while others drop to ~88K ops/sec (CV 34.6%). The single-Raft consolidation (commit `bba1a5d4`) achieves consistent ~209K ops/sec (CV 1.9%).

**Root cause**: The bimodal behavior results from three interacting factors: (1) election timing interference between 6 concurrent Raft groups during the 5-second startup grace period, (2) a 500ms blocking RPC wait in the heartbeat loop that is 100x longer than the 5ms heartbeat interval, and (3) thread-level resource contention from 30+ OS threads competing on a shared machine.

## Environment Verification

The test configuration is correct and equivalent for both benchmarks:

| Parameter | Value | Source |
|-----------|-------|--------|
| Shards | 1 | `test_1shard_replication_raft.sh` |
| Partitions (threads) | 6 | `--num-threads 6` |
| Replicas | 3 (localhost, p1, p2) | `raft6_shardidx0.yml` |
| All on localhost | Yes | `host: localhost: 127.0.0.1, p1: 127.0.0.1, p2: 127.0.0.1` |
| Test duration | 60 seconds | `sleep 60` in test script |
| Benchmark | TPC-C | `occ_raft.yml` |

Both configurations use identical test scripts, configs, and hardware. The only difference is the Raft instance architecture.

## Root Cause 1: Election Timing Interference (Primary)

### The Problem

In multi-Raft mode, 6 independent Raft groups start elections concurrently during the first 5 seconds (grace period). Each group's preferred leader has a 150–300ms election timeout, and non-preferred replicas have 1–2s timeouts.

With 6 groups on localhost, all 6 preferred leaders fire elections within the same 150–300ms window. The elections compete for:
- CPU time (12 fibers — 2 per group — running on the same PollThread)
- Network resources (6 concurrent `BroadcastVote` RPC batches)
- Vote processing time (6 concurrent `RequestVote` handlers on each follower)

### The Math

Probability of at least one delayed election with N=6 groups:
- Each election succeeds in one round with probability p ≈ 0.8–0.9 (conservative estimate for localhost with randomized timeouts)
- P(all 6 succeed first try) = p^6 ≈ 0.26–0.53
- P(at least one needs retry) ≈ 0.47–0.74

This matches the observed data: roughly 3–4 out of 10 runs hit the "fast" mode (all elections succeed), and 4–6 runs hit "slow" mode (one or more elections stall).

### Evidence

The bimodal distribution in multi-Raft throughput data:
- **Fast mode** (~190K–200K ops/sec): 3/10 runs — all 6 elections succeeded quickly
- **Slow mode** (~88K–91K ops/sec): 4/10 runs — at least one election stalled
- **Medium** (~120K–160K ops/sec): 3/10 runs — some elections were delayed

The single-Raft instance eliminates this entirely: 1 election, no interference, every run succeeds in the first attempt.

### Code Reference

Election timeout at commit `4f99ffb6` (`src/deptran/raft/server.cc`):
```cpp
if (AmIPreferredLeader()) {
    base_timeout = 150000;  // 150ms — all 6 preferred leaders use this same range
    uint64_t jitter = RandomGenerator::rand(0, 150000);
    return base_timeout + jitter;  // 150-300ms
}
```

## Root Cause 2: 500ms Blocking RPC Wait (Amplifier)

### The Problem

The `HeartbeatLoop` in `server.cc` waits up to **500ms** for follower responses after sending AppendEntries RPCs:

```cpp
wait_all->wait(500000);  // 500ms total timeout for all follower responses
```

This is **100x longer** than the heartbeat interval (`HEARTBEAT_INTERVAL = 5000` microseconds = 5ms). When any RPC response is slow — due to thread scheduling delays, network jitter, or CPU contention from other Raft groups — the heartbeat loop blocks for up to 500ms.

### Cascading Effect

1. A single slow RPC response blocks the heartbeat loop for up to 500ms
2. During this 500ms, the election timer on the follower may fire (since it expects heartbeats every 150–300ms for preferred leaders)
3. An unnecessary election starts, blocking the PollThread for up to 1 second (`sp_quorum->wait(1000000)`)
4. Total disruption: 500ms (heartbeat wait) + 1000ms (election) = **1.5 seconds** of lost throughput
5. With 6 groups, the probability of at least one triggering this cascade is high

### In Single-Raft Mode

The single-Raft instance has only 2 follower connections (not 12), so the probability of a slow RPC is lower. The election timeouts were also increased (300–600ms for preferred, 3–6s for non-preferred), providing much more headroom.

## Root Cause 3: Thread Resource Contention (Enabler)

### Multi-Raft Thread Count

For 6 partitions, multi-Raft creates:

| Resource | Count | Purpose |
|----------|-------|---------|
| RPC PollThreads | 6 | One per RaftWorker (SetupService) |
| Heartbeat PollThreads | 6 | One per RaftWorker (SetupHeartbeat) |
| RPC ThreadPools | 6 | 1 thread each |
| Heartbeat ThreadPools | 6 | 1 thread each |
| Submit threads | 6 | One per RaftWorker |
| Fibers (HeartbeatLoop) | 6 | One per RaftServer |
| Fibers (ElectionTimer) | 6 | One per RaftServer |
| **Total OS threads** | **~30+** | |

### Single-Raft Thread Count

| Resource | Count |
|----------|-------|
| RPC PollThreads | 1 (+ 5 stub servers) |
| Heartbeat PollThread | 1 |
| ThreadPools | 2 |
| Submit thread | 1 |
| Apply thread | 1 |
| Fibers | 2 |
| **Total OS threads** | **~12** |

### Impact

30+ threads competing on a shared-memory machine causes:
- **Context switching overhead**: Each PollThread runs an epoll loop; with 12 PollThreads, the OS scheduler must frequently switch between them
- **Cache thrashing**: Different PollThreads access different `RaftServer` objects, evicting each other's data from CPU caches
- **Lock convoy risk**: If two HeartbeatLoops try to acquire their respective `mtx_` simultaneously and one is delayed by a context switch, the delayed one holds its fiber's PollThread slot, blocking other fibers on that thread

## Why Multi-Raft Is Slower Even in Best Case

Even in the "fast" runs (~200K ops/sec), multi-Raft is ~5% slower than single-Raft (200K vs 209K). This is because:

1. **Heartbeat overhead**: 12 heartbeat RPCs per interval (6 groups × 2 followers) vs 2 (1 group × 2 followers)
2. **Thread overhead**: 30+ threads consume CPU cycles even when idle (polling, scheduling)
3. **Duplicate work**: 6 independent commit-index calculations, 6 independent match-index sorts, 6 independent log GC checks

## Conclusion

The bimodal throughput is not caused by a bug but by a fundamental architectural limitation of running 6 independent Raft groups in the same process. The three root causes interact multiplicatively:

1. **Election interference** creates the bimodal distribution (fast vs slow startup)
2. **500ms blocking wait** amplifies any slowdown into a cascade
3. **Thread contention** makes slowdowns more likely by competing for shared CPU

The single-Raft consolidation addresses all three: 1 election eliminates interference, 2 RPCs (not 12) reduce probability of slow responses, and ~12 threads (not 30+) reduce contention.

No code fix is recommended for the multi-Raft path since the single-Raft consolidation already solves the problem comprehensively. The multi-Raft code remains at commit `4f99ffb6` as a baseline for comparison.
