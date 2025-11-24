# Quick Start: Getting Baseline Metrics from Mako

This is a quick reference for getting baseline performance metrics from Mako.

## TL;DR - Get Metrics in 3 Steps

```bash
# 1. Build
make -j32

# 2. Run benchmark (60 seconds)
bash examples/test_1shard_replication.sh

# 3. Extract metrics
./extract_metrics.sh test_1shard_replication.sh_shard0-localhost-6.log
```

## What You'll See

The `extract_metrics.sh` script will show you:

```
=== CORE PERFORMANCE METRICS ===
Runtime: 60.2 sec
Total Commits: 2,450,123
Aggregate Throughput: 40,685.5 ops/sec
Avg Per-Core Throughput: 6,780.9 ops/sec/core

=== LATENCY METRICS ===
Average Latency: 1.47 ms
Persist Latency: 1.52 ms

=== ABORT METRICS ===
Aggregate Abort Rate: 234.5 aborts/sec
Avg Per-Core Abort Rate: 39.1 aborts/sec/core

=== TPC-C TRANSACTION METRICS ===
--- NewOrder Transaction ---
  NewOrder_local_commit_latency: 1.82 ms
  NewOrder_local_abort_ratio: 0.05
  NewOrder_remote_ratio: 15.3 %
  NewOrder_remote_abort_ratio: 8.2 %
```

## Available Test Configurations

### 1. Single Shard with Replication (Recommended for Baseline)
```bash
bash examples/test_1shard_replication.sh
# Output: test_1shard_replication.sh_shard0-localhost-6.log
```

**Configuration:**
- 1 shard
- 6 worker threads
- 3 replicas (localhost, p1, p2)
- Runs for 60 seconds

### 2. Two Shards with Replication
```bash
bash examples/test_2shard_replication.sh
# Output: test_2shard_replication.sh_rrr_shard0-localhost.log
#         test_2shard_replication.sh_rrr_shard1-localhost.log
```

**Configuration:**
- 2 shards
- 6 worker threads per shard
- 3 replicas per shard
- Runs for 70 seconds

### 3. Two Shards without Replication (Simpler)
```bash
bash examples/test_2shard_no_replication.sh
# Output: test_2shard_no_replication.sh_shard0.log
#         test_2shard_no_replication.sh_shard1.log
```

**Configuration:**
- 2 shards
- 6 worker threads per shard
- No replication
- Runs for 60 seconds

## Extracting Metrics from Multiple Shards

For multi-shard tests:

```bash
# Run 2-shard test
bash examples/test_2shard_replication.sh

# Extract metrics from both shards
./extract_metrics.sh test_2shard_replication.sh_rrr_shard0-localhost.log > shard0_metrics.txt
./extract_metrics.sh test_2shard_replication.sh_rrr_shard1-localhost.log > shard1_metrics.txt

# View combined results
cat shard0_metrics.txt shard1_metrics.txt
```

## Key Metrics Explained

### Throughput
- **agg_throughput**: Total transactions per second across all threads
- **Higher is better**
- Typical range: 20,000 - 100,000+ ops/sec depending on configuration

### Latency
- **avg_latency**: Average transaction completion time in milliseconds
- **Lower is better**
- Typical range: 1-5 ms for local transactions, 10-50 ms for distributed

### Abort Rate
- **agg_abort_rate**: Number of aborted transactions per second
- **Lower is better**
- Should be < 5% of throughput for good performance

### Remote Abort Ratio
- **NewOrder_remote_abort_ratio**: Percentage of distributed transactions that abort
- **Lower is better**
- Target: < 20% for 1 shard, < 40% for 2 shards

## Comparing Different Configurations

Create a comparison script:

```bash
#!/bin/bash
# compare_configs.sh

echo "=== 1 Shard Baseline ==="
bash examples/test_1shard_replication.sh
./extract_metrics.sh test_1shard_replication.sh_shard0-localhost-6.log | grep "Aggregate Throughput"
echo ""

echo "=== 2 Shards Baseline ==="
bash examples/test_2shard_replication.sh
./extract_metrics.sh test_2shard_replication.sh_rrr_shard0-localhost.log | grep "Aggregate Throughput"
./extract_metrics.sh test_2shard_replication.sh_rrr_shard1-localhost.log | grep "Aggregate Throughput"
```

## Manual Metrics Extraction (Without Script)

If you prefer to extract metrics manually:

```bash
# Throughput
grep "agg_throughput:" logfile.log

# Latency
grep "avg_latency:" logfile.log

# Abort rate
grep "agg_abort_rate:" logfile.log

# Transaction-specific metrics
grep "NewOrder" logfile.log
```

## Troubleshooting

### No log file generated
- Check if the test script ran successfully
- Look for error messages in the console output
- Ensure all processes started (check with `ps aux | grep dbtest`)

### Metrics not in log file
- The benchmark may not have completed
- Check the end of the log file for "benchmark statistics"
- Ensure the test ran for at least 30 seconds

### Very low throughput
- Check system resources (CPU, memory)
- Reduce number of threads if system is overloaded
- Check for errors in the log file

### High abort rate
- Normal for multi-shard configurations
- Check `NewOrder_remote_abort_ratio` - should be reasonable
- May indicate configuration issues if > 50%

## Next Steps

1. **Collect baseline metrics** using the scripts above
2. **Document your results** in a spreadsheet or text file
3. **Compare configurations** (1 shard vs 2 shards, with/without replication)
4. **Read the full guide** in `BENCHMARKING_GUIDE.md` for advanced usage

## Additional Resources

- **Full Benchmarking Guide**: `BENCHMARKING_GUIDE.md`
- **Main README**: `README.md`
- **Documentation**: `doc/` directory
- **Example Scripts**: `examples/` directory
- **Configuration Files**: `src/mako/config/` and `config/` directories
