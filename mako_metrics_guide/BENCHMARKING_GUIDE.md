# Mako Benchmarking Guide - Getting Baseline Metrics

This guide explains how to run Mako benchmarks and extract baseline performance metrics (throughput, latency, etc.) without creating custom scripts.

## Overview

Mako has a built-in benchmarking system that outputs detailed performance metrics. The main benchmark executable is `dbtest`, which runs TPC-C workloads and reports comprehensive statistics.

## Quick Start - Running Existing Benchmarks

### 1. Build the System

```bash
cd /home/marumalla/mako
make -j32
```

### 2. Run Pre-configured Tests

The easiest way to get baseline metrics is to use the existing test scripts:

#### Single Shard with Replication (Recommended for Baseline)
```bash
bash examples/test_1shard_replication.sh
```

This runs for 60 seconds and outputs metrics to log files.

#### Two Shards with Replication
```bash
bash examples/test_2shard_replication.sh
```

This runs for 70 seconds with 2 shards.

#### Two Shards without Replication (Simpler)
```bash
bash examples/test_2shard_no_replication.sh
```

### 3. Check the Output Logs

After running a test, check the generated log files:

```bash
# For 1-shard test:
cat test_1shard_replication.sh_shard0-localhost-6.log

# For 2-shard test:
cat test_2shard_replication.sh_rrr_shard0-localhost.log
cat test_2shard_replication.sh_rrr_shard1-localhost.log
```

## Understanding the Metrics Output

When `dbtest` completes, it outputs detailed benchmark statistics. Here are the key metrics:

### Core Performance Metrics

From `bench.cc` (lines 659-678), the system outputs:

```
--- benchmark statistics ---
runtime: <seconds> sec
n_commits: <total_committed_transactions>
agg_nosync_throughput: <ops/sec> ops/sec
avg_nosync_per_core_throughput: <ops/sec/core> ops/sec/core
agg_throughput: <ops/sec> ops/sec
avg_per_core_throughput: <ops/sec/core> ops/sec/core
agg_persist_throughput: <ops/sec> ops/sec
avg_per_core_persist_throughput: <ops/sec/core> ops/sec/core
avg_latency: <milliseconds> ms
avg_persist_latency: <milliseconds> ms
agg_abort_rate: <aborts/sec> aborts/sec
avg_per_core_abort_rate: <aborts/sec/core> aborts/sec/core
```

### TPC-C Specific Metrics

For TPC-C workload (lines 680-698):

```
NewOrder_local_commit_latency: <ms> ms
NewOrder_local_abort_latency: <ms> ms
NewOrder_local_abort_ratio: <ratio>
NewOrder_remote_ratio: <percentage> %
NewOrder_remote_abort_ratio: <percentage> %
NewOrder_remote_commit_latency: <ms> ms
NewOrder_remote_abort_latency: <ms> ms

Payment_local_commit_latency: <ms> ms
Payment_remote_ratio: <percentage> %
Payment_remote_abort_ratio: <percentage> %
```

### Key Metrics Explained

1. **agg_throughput**: Total transactions per second across all threads
2. **avg_latency**: Average transaction latency in milliseconds
3. **agg_persist_throughput**: Throughput including persistence to disk
4. **agg_abort_rate**: Number of aborted transactions per second
5. **NewOrder_remote_abort_ratio**: Percentage of distributed transactions that abort (important for multi-shard)

## Manual Benchmark Execution

### Running dbtest Directly

You can run `dbtest` manually for custom configurations:

```bash
# Single shard, 6 threads, with replication
./build/dbtest \
  --num-threads 6 \
  --shard-index 0 \
  --shard-config src/mako/config/local-shards1-warehouses6.yml \
  -F config/1leader_2followers/paxos6_shardidx0.yml \
  -F config/occ_paxos.yml \
  -P localhost \
  --is-replicated
```

### Command Line Arguments

From `dbtest.cc`:

- `--num-threads <N>`: Number of worker threads (default: varies)
- `--shard-index <N>`: Index of this shard (0, 1, 2, ...)
- `--shard-config <file>`: Shard configuration YAML file
- `-F <file>`: Paxos configuration file (can specify multiple)
- `-P <name>`: Cluster role (localhost, p1, p2, learner)
- `--is-micro`: Enable micro-benchmark mode
- `--is-replicated`: Enable replication mode

### Configuration Files

#### Shard Configurations

Located in `src/mako/config/`:

- `local-shards1-warehouses6.yml`: 1 shard, 6 warehouses (threads)
- `local-shards2-warehouses6.yml`: 2 shards, 6 warehouses each
- `local-shards10-warehouses24.yml`: 10 shards, 24 warehouses each

The number of warehouses should match the number of threads.

#### Paxos Configurations

Located in `config/1leader_2followers/`:

- `paxos6_shardidx0.yml`: Paxos config for shard 0 with 6 threads
- `paxos6_shardidx1.yml`: Paxos config for shard 1 with 6 threads

## Running Custom Benchmarks

### Example: 1 Shard, 8 Threads, 30 Seconds

```bash
# Start the leader
./build/dbtest \
  --num-threads 8 \
  --shard-index 0 \
  --shard-config src/mako/config/local-shards1-warehouses8.yml \
  -F config/1leader_2followers/paxos8_shardidx0.yml \
  -F config/occ_paxos.yml \
  -P localhost \
  --is-replicated &

# Wait for completion (dbtest runs for a configured duration)
wait
```

### Example: 2 Shards, 6 Threads Each

```bash
# Start shard 0 leader
./build/dbtest --num-threads 6 --shard-index 0 \
  --shard-config src/mako/config/local-shards2-warehouses6.yml \
  -F config/1leader_2followers/paxos6_shardidx0.yml \
  -F config/occ_paxos.yml -P localhost --is-replicated > shard0.log 2>&1 &

# Start shard 1 leader
./build/dbtest --num-threads 6 --shard-index 1 \
  --shard-config src/mako/config/local-shards2-warehouses6.yml \
  -F config/1leader_2followers/paxos6_shardidx1.yml \
  -F config/occ_paxos.yml -P localhost --is-replicated > shard1.log 2>&1 &

# Wait for completion
sleep 60
pkill -9 dbtest

# Check results
grep "agg_throughput" shard0.log shard1.log
grep "avg_latency" shard0.log shard1.log
```

## Extracting Metrics from Logs

### Using grep to Extract Key Metrics

```bash
# Extract throughput
grep "agg_throughput:" logfile.log

# Extract latency
grep "avg_latency:" logfile.log

# Extract abort ratio
grep "NewOrder_remote_abort_ratio:" logfile.log

# Extract all benchmark statistics
grep -A 20 "--- benchmark statistics ---" logfile.log
```

### Example Output

```
agg_throughput: 45234.5 ops/sec
avg_latency: 2.34 ms
NewOrder_remote_abort_ratio: 12.5 %
```

## Benchmark Flags in CMakeLists.txt

The system supports several compile-time flags for advanced benchmarking:

### Available Flags

From `CMakeLists.txt` (lines 201-203):

```cmake
option(MEGA_BENCHMARK "Enable MEGA benchmark" OFF)
option(MEGA_BENCHMARK_MICRO "Enable MEGA benchmark micro" OFF)
option(TRACKING_LATENCY "Enable latency tracking" OFF)
```

### Enabling Flags

To enable latency tracking:

```bash
cd build
cmake -DTRACKING_LATENCY=ON ..
make -j32
```

## Using the Python Runner (Advanced)

For more complex experiments, use `run.py`:

```bash
# Run with configuration files
./run.py -f config/1c1s1p.yml -f config/tpcc.yml

# The script will:
# 1. Start servers and clients
# 2. Run the benchmark
# 3. Collect statistics
# 4. Output results
```

## Baseline Metrics Collection Strategy

### Recommended Approach

1. **Start Simple**: Run 1-shard tests first
   ```bash
   bash examples/test_1shard_replication.sh
   ```

2. **Scale Up**: Run 2-shard tests
   ```bash
   bash examples/test_2shard_replication.sh
   ```

3. **Extract Metrics**: Parse the log files
   ```bash
   grep "agg_throughput:" *.log
   grep "avg_latency:" *.log
   grep "NewOrder_remote_abort_ratio:" *.log
   ```

4. **Document Results**: Create a baseline metrics file
   ```bash
   cat > baseline_metrics.txt << EOF
   Configuration: 1 shard, 6 threads, with replication
   Throughput: $(grep "agg_throughput:" test_1shard_replication.sh_shard0-localhost-6.log | awk '{print $2, $3}')
   Latency: $(grep "avg_latency:" test_1shard_replication.sh_shard0-localhost-6.log | awk '{print $2, $3}')
   EOF
   ```

### Metrics to Collect

For baseline comparison, collect:

1. **Throughput Metrics**:
   - `agg_throughput` (total ops/sec)
   - `avg_per_core_throughput` (ops/sec per thread)

2. **Latency Metrics**:
   - `avg_latency` (average transaction latency)
   - Per-transaction type latencies (NewOrder, Payment, etc.)

3. **Reliability Metrics**:
   - `agg_abort_rate` (aborts per second)
   - `NewOrder_remote_abort_ratio` (distributed transaction abort rate)

4. **System Metrics**:
   - `runtime` (total execution time)
   - `n_commits` (total committed transactions)

## Troubleshooting

### No Metrics Output

If you don't see metrics, ensure:
1. The test ran long enough (at least 30 seconds)
2. Check for errors in the log file
3. Verify all processes started correctly

### Low Throughput

Common causes:
1. Too few threads (increase `--num-threads`)
2. System resource constraints (check CPU/memory)
3. Network issues (for multi-shard setups)

### High Abort Rate

If abort rate is high:
1. Check `NewOrder_remote_abort_ratio` - should be < 20% for 1 shard, < 40% for 2 shards
2. May indicate contention or configuration issues
3. Review shard configuration and data distribution

## Summary

The Mako system already has comprehensive benchmarking built-in:

1. **Use existing test scripts** in `examples/` directory
2. **Check log files** for detailed metrics output
3. **Key metrics** are automatically printed at the end of each run
4. **No custom scripts needed** - the system outputs everything you need

The metrics are printed to stderr by the `bench_runner::~bench_runner()` destructor in `bench.cc`, so they appear at the end of each benchmark run.
