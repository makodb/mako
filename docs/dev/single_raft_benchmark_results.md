# Single Raft Instance Benchmark Results

## Configuration
- **Commit**: `bba1a5d4` (Single Raft Instance: Throughput Issue Fixed)
- **Branch**: `mako-krish-new`
- **Test**: `./ci/ci_mako_raft.sh shard1ReplicationRaft`
- **Build**: `make clean && make mako-raft -j32`
- **Date**: 2026-02-27

## Raw Results (10 runs)

| Run | Throughput (ops/sec) | Replay Batch |
|-----|---------------------|--------------|
| 1   | 215,503             | 11,720       |
| 2   | 207,083             | 12,018       |
| 3   | 209,322             | 11,514       |
| 4   | 216,419             | 9,432        |
| 5   | 210,284             | 9,708        |
| 6   | 208,124             | 11,906       |
| 7   | 205,792             | 9,165        |
| 8   | 208,597             | 11,642       |
| 9   | 205,853             | 9,030        |
| 10  | 204,853             | 12,346       |

## Summary Statistics — Throughput

| Metric | Value |
|--------|-------|
| Mean   | 209,183 ops/sec |
| Median | 208,361 ops/sec |
| Min    | 204,853 ops/sec |
| Max    | 216,419 ops/sec |
| Stdev  | 3,955 ops/sec |
| CV     | 1.9% |

## Summary Statistics — Replay Batch

| Metric | Value |
|--------|-------|
| Mean   | 10,848 |
| Median | 11,578 |
| Min    | 9,030 |
| Max    | 12,346 |
| Stdev  | 1,334 |
