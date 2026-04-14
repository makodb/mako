# Multiple Raft Instances Benchmark Results

## Configuration
- **Commit**: `4f99ffb6` (Fact-check thesis: correct test duration claims and gap decomposition)
- **Branch**: `mako-krish-new` (detached HEAD at old commit)
- **Test**: `./ci/ci_mako_raft.sh shard1ReplicationRaft`
- **Build**: `make clean && make mako-raft -j32`
- **Date**: 2026-02-27

## Raw Results (10 runs)

| Run | Throughput (ops/sec) | Replay Batch |
|-----|---------------------|--------------|
| 1   | 190,831             | 12,069       |
| 2   | 91,464              | 1,448        |
| 3   | 199,653             | 8,277        |
| 4   | 160,187             | 7,330        |
| 5   | 88,979              | 1,285        |
| 6   | 88,078              | 976          |
| 7   | 89,871              | 1,365        |
| 8   | 198,647             | 10,387       |
| 9   | 149,909             | 6,296        |
| 10  | 121,899             | 3,221        |

## Summary Statistics — Throughput

| Metric | Value |
|--------|-------|
| Mean   | 137,952 ops/sec |
| Median | 135,904 ops/sec |
| Min    | 88,078 ops/sec |
| Max    | 199,653 ops/sec |
| Stdev  | 47,774 ops/sec |
| CV     | 34.6% |

## Summary Statistics — Replay Batch

| Metric | Value |
|--------|-------|
| Mean   | 5,265 |
| Median | 4,759 |
| Min    | 976 |
| Max    | 12,069 |
| Stdev  | 4,152 |

## Observations

The multi-Raft instance configuration shows a bimodal distribution:
- **High runs** (~190K-200K): 3 out of 10 runs achieved near-peak throughput
- **Low runs** (~88K-91K): 4 out of 10 runs showed roughly half the peak
- **Medium runs** (~120K-160K): 3 out of 10 runs fell in the middle

This bimodal behavior is likely caused by election timing variance: with 6 independent Raft groups, some runs may experience split elections or delayed leader establishment, reducing overall throughput during those periods.
