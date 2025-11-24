# Mako Baseline Metrics - Complete Guide

This directory contains everything you need to collect and analyze baseline performance metrics from Mako.

## 📋 Quick Start (3 Commands)

```bash
# 1. Build the system
make -j32

# 2. Collect baseline metrics (runs for ~3 minutes)
./collect_baseline_metrics.sh

# 3. View results
cat baseline_results_*/SUMMARY.txt
```

## 📁 Files in This Directory

### Scripts
- **`collect_baseline_metrics.sh`** - Automated baseline collection (recommended)
- **`extract_metrics.sh`** - Extract metrics from a single log file
- **`examples/test_1shard_replication.sh`** - Run 1-shard benchmark
- **`examples/test_2shard_replication.sh`** - Run 2-shard benchmark

### Documentation
- **`QUICK_START_BENCHMARKS.md`** - Quick reference guide (start here!)
- **`BENCHMARKING_GUIDE.md`** - Comprehensive benchmarking documentation
- **`README.md`** - Main project README

## 🚀 Three Ways to Get Metrics

### Option 1: Automated Collection (Easiest)

Run the automated script to collect all baseline metrics:

```bash
./collect_baseline_metrics.sh
```

This will:
- Run 1-shard and 2-shard benchmarks
- Extract all metrics automatically
- Create a summary report
- Generate a CSV file for spreadsheet analysis

**Output:**
- `baseline_results_TIMESTAMP/SUMMARY.txt` - Quick summary
- `baseline_results_TIMESTAMP/metrics.csv` - CSV for Excel/Sheets
- `baseline_results_TIMESTAMP/*_metrics.txt` - Detailed metrics per shard
- `baseline_results_TIMESTAMP/*.log` - Full benchmark logs

### Option 2: Manual Single Test

Run a specific test and extract metrics:

```bash
# Run test
bash examples/test_1shard_replication.sh

# Extract metrics
./extract_metrics.sh test_1shard_replication.sh_shard0-localhost-6.log
```

### Option 3: Direct Log Analysis

Run a test and manually grep for metrics:

```bash
# Run test
bash examples/test_1shard_replication.sh

# View key metrics
grep "agg_throughput:" test_1shard_replication.sh_shard0-localhost-6.log
grep "avg_latency:" test_1shard_replication.sh_shard0-localhost-6.log
```

## 📊 Understanding the Metrics

### Core Metrics

| Metric | Description | Good Value |
|--------|-------------|------------|
| **Aggregate Throughput** | Total transactions/second | Higher is better (20K-100K+) |
| **Average Latency** | Transaction completion time (ms) | Lower is better (1-5 ms) |
| **Abort Rate** | Failed transactions/second | Lower is better (< 5% of throughput) |
| **Remote Abort Ratio** | % of distributed txns that abort | < 20% (1 shard), < 40% (2 shards) |

### Example Output

```
=== CORE PERFORMANCE METRICS ===
Runtime: 60.2 sec
Total Commits: 2,450,123
Aggregate Throughput: 40,685.5 ops/sec
Avg Per-Core Throughput: 6,780.9 ops/sec/core

=== LATENCY METRICS ===
Average Latency: 1.47 ms

=== ABORT METRICS ===
Aggregate Abort Rate: 234.5 aborts/sec

=== TPC-C TRANSACTION METRICS ===
--- NewOrder Transaction ---
  NewOrder_remote_abort_ratio: 8.2 %
```

## 🔧 Available Test Configurations

### 1. Single Shard with Replication
```bash
bash examples/test_1shard_replication.sh
```
- **Configuration**: 1 shard, 6 threads, 3 replicas
- **Duration**: 60 seconds
- **Use case**: Baseline single-shard performance

### 2. Two Shards with Replication
```bash
bash examples/test_2shard_replication.sh
```
- **Configuration**: 2 shards, 6 threads each, 3 replicas per shard
- **Duration**: 70 seconds
- **Use case**: Distributed transaction performance

### 3. Two Shards without Replication
```bash
bash examples/test_2shard_no_replication.sh
```
- **Configuration**: 2 shards, 6 threads each, no replication
- **Duration**: 60 seconds
- **Use case**: Simpler multi-shard setup

## 📈 Analyzing Results

### Compare Configurations

```bash
# Collect all baselines
./collect_baseline_metrics.sh

# View CSV in spreadsheet
# Open: baseline_results_*/metrics.csv

# Or compare in terminal
cat baseline_results_*/SUMMARY.txt
```

### Key Comparisons

1. **Single Shard vs Multi-Shard**
   - Compare throughput scaling
   - Check latency increase
   - Monitor abort ratio changes

2. **With vs Without Replication**
   - Measure replication overhead
   - Check consistency guarantees
   - Evaluate fault tolerance

3. **Different Thread Counts**
   - Modify thread count in test scripts
   - Measure scalability
   - Find optimal configuration

## 🐛 Troubleshooting

### Problem: No metrics in output
**Solution:**
```bash
# Check if benchmark completed
grep "benchmark statistics" logfile.log

# If not found, check for errors
tail -50 logfile.log
```

### Problem: Very low throughput
**Solution:**
```bash
# Check system resources
htop

# Reduce thread count if needed
# Edit test script and change trd=6 to trd=4
```

### Problem: High abort rate
**Solution:**
- Check `NewOrder_remote_abort_ratio` in output
- Should be < 20% for 1 shard, < 40% for 2 shards
- Higher values may indicate configuration issues

### Problem: Hanging processes
**Solution:**
```bash
# Kill all test processes
pkill -9 dbtest
sleep 2

# Re-run the test
```

## 📚 Additional Documentation

- **`QUICK_START_BENCHMARKS.md`** - Quick reference for common tasks
- **`BENCHMARKING_GUIDE.md`** - Detailed benchmarking documentation
- **`doc/run.md`** - Advanced distributed testing
- **`doc/config.md`** - Configuration file reference

## 🎯 Recommended Workflow

1. **Initial Setup**
   ```bash
   make -j32
   ```

2. **Collect Baseline**
   ```bash
   ./collect_baseline_metrics.sh
   ```

3. **Review Results**
   ```bash
   cat baseline_results_*/SUMMARY.txt
   ```

4. **Document Findings**
   - Save the `baseline_results_*` directory
   - Import `metrics.csv` to spreadsheet
   - Compare with paper results (README.md)

5. **Iterate**
   - Modify configurations
   - Re-run specific tests
   - Compare with baseline

## 📝 Example: Complete Baseline Collection

```bash
# Step 1: Build
cd /home/marumalla/mako
make -j32

# Step 2: Collect all baselines
./collect_baseline_metrics.sh

# Step 3: View summary
cat baseline_results_*/SUMMARY.txt

# Step 4: Open CSV in spreadsheet
# File: baseline_results_*/metrics.csv

# Step 5: Compare with paper results
# Paper claims: 960K TPS (1 shard), 3.66M TPS (10 shards)
# Your baseline: Check SUMMARY.txt
```

## 🔍 Where Metrics Come From

The metrics are automatically generated by the Mako system:

- **Source code**: `src/mako/benchmarks/bench.cc` (lines 659-698)
- **Output location**: stderr at end of benchmark run
- **Format**: Plain text, easily parseable
- **Completeness**: Includes throughput, latency, abort rates, and per-transaction metrics

**You don't need to create any custom instrumentation** - the system already outputs everything you need!

## 🎓 Understanding the Benchmarking System

### How It Works

1. **`dbtest`** executable runs TPC-C workload
2. Worker threads execute transactions
3. Metrics are collected during execution
4. Results are printed at the end (in `bench_runner` destructor)
5. Scripts capture output to log files

### Key Components

- **`dbtest`**: Main benchmark executable
- **TPC-C**: Industry-standard database benchmark
- **Paxos**: Replication protocol for fault tolerance
- **Masstree**: In-memory storage engine
- **RocksDB**: Persistent storage backend

## 🚦 Next Steps

After collecting baseline metrics:

1. **Compare with paper results** (README.md)
2. **Try different configurations** (modify test scripts)
3. **Scale up** (increase shards/threads)
4. **Profile performance** (see `doc/profile.md`)
5. **Deploy distributed** (see `doc/ec2.md`)

---

**Need Help?**
- Check `QUICK_START_BENCHMARKS.md` for quick reference
- Read `BENCHMARKING_GUIDE.md` for detailed info
- Review example scripts in `examples/` directory
- Check documentation in `doc/` directory
