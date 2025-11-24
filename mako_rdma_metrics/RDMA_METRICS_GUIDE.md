# RDMA Implementation Metrics Guide
## Implementing RDMA in sRPC Framework (from eRPC)

## 🎯 Your Goal
Implement RDMA functionality within the sRPC framework by taking functionality from eRPC to improve:
1. **CPU Utilization** (reduce CPU overhead)
2. **Latency** (reduce network latency)

**You're right:** Geo-distributed testing is NOT needed for this work. Focus on **single-datacenter, low-latency RPC performance**.

---

## 📊 Key Metrics to Focus On

### 1. **RPC Latency** (PRIMARY METRIC)
**Why it matters:** RDMA's main benefit is ultra-low latency through kernel bypass.

**Target Improvements:**
- **Current (sRPC/TCP):** ~10-50 μs
- **Target (RDMA):** ~1-2 μs
- **Goal:** 5-25x latency reduction

**How to measure:**
```bash
# Benchmark RPC latency with rpcbench
./build/rpcbench -s 0.0.0.0:8848 -f &
./build/rpcbench -c localhost:8848 -f -n 30 -o 1

# Look for:
# - Average latency (μs)
# - Min/Max latency
# - P50, P99, P999 latencies
```

**What to track:**
```
Current sRPC:
  Average latency: ~15-30 μs
  P99 latency: ~50-100 μs

After RDMA implementation:
  Average latency: ~1-3 μs (target)
  P99 latency: ~5-10 μs (target)
```

---

### 2. **CPU Utilization** (PRIMARY METRIC)
**Why it matters:** RDMA uses kernel bypass, reducing CPU overhead for network I/O.

**Target Improvements:**
- **Current (sRPC/TCP):** 60-80% CPU for network processing
- **Target (RDMA):** 20-40% CPU for network processing
- **Goal:** 50% reduction in CPU usage at same throughput

**How to measure:**
```bash
# Monitor CPU during benchmark
# Terminal 1: Start server
./build/rpcbench -s 0.0.0.0:8848 -w 8 &
SERVER_PID=$!

# Terminal 2: Monitor CPU
top -p $SERVER_PID -d 1

# Terminal 3: Run client
./build/rpcbench -c localhost:8848 -t 8 -o 5000 -n 60

# Or use perf for detailed CPU profiling
perf stat -p $SERVER_PID -e cycles,instructions,cache-misses sleep 30
```

**What to track:**
```
Metric                  | sRPC (TCP)  | RDMA (Target)
------------------------|-------------|---------------
CPU % at 100K RPS       | 70-80%      | 30-40%
CPU % at 500K RPS       | 95-100%     | 60-70%
Cycles per request      | ~5000       | ~1000-2000
Context switches/sec    | High        | Low (kernel bypass)
```

---

### 3. **Throughput** (SECONDARY METRIC)
**Why it matters:** RDMA should handle more requests/sec with same CPU.

**Target Improvements:**
- **Current (sRPC/TCP):** 100K-200K RPS
- **Target (RDMA):** 500K-1M+ RPS
- **Goal:** 3-5x throughput improvement

**How to measure:**
```bash
# Max throughput test
./build/rpcbench -s 0.0.0.0:8848 -w 16 &
./build/rpcbench -c localhost:8848 -t 16 -o 10000 -n 60

# Look for:
# - client qps: XXX (requests per second)
# - Average: XXX reqs/s
```

**What to track:**
```
Configuration          | sRPC (TCP)  | RDMA (Target)
-----------------------|-------------|---------------
8 threads, 1K msgs     | 150K RPS    | 500K+ RPS
16 threads, 1K msgs    | 200K RPS    | 800K+ RPS
Fast mode, small msgs  | 200K RPS    | 1M+ RPS
```

---

### 4. **Message Size Impact** (ANALYSIS METRIC)
**Why it matters:** RDMA benefits vary with message size (zero-copy for large messages).

**How to measure:**
```bash
# Test different message sizes
for size in 10 100 1024 4096 8192; do
  echo "Testing message size: $size bytes"
  ./build/rpcbench -c localhost:8848 -b $size -n 10 | grep "Average"
done
```

**What to track:**
```
Message Size | sRPC Latency | RDMA Latency | Improvement
-------------|--------------|--------------|-------------
10 bytes     | 15 μs        | 1.5 μs       | 10x
100 bytes    | 18 μs        | 2 μs         | 9x
1 KB         | 25 μs        | 3 μs         | 8x
4 KB         | 45 μs        | 5 μs         | 9x
8 KB         | 80 μs        | 8 μs         | 10x
```

---

### 5. **System Overhead Metrics** (DIAGNOSTIC)

**CPU Cycles per Request:**
```bash
# Use perf to measure CPU efficiency
perf stat -e cycles,instructions,cache-misses \
  ./build/rpcbench -c localhost:8848 -n 30
```

**Context Switches:**
```bash
# Monitor context switches (should be lower with RDMA)
vmstat 1 30  # While benchmark is running

# Look for:
# - cs (context switches per second)
# - in (interrupts per second)
```

**Network Stack Overhead:**
```bash
# System calls (should be near-zero with RDMA kernel bypass)
strace -c -p $SERVER_PID

# Look for:
# - sendto/recvfrom calls (high in TCP, low in RDMA)
# - poll/epoll calls
```

---

## 🧪 Recommended Benchmark Suite

### Benchmark 1: Latency Comparison (Most Important)
```bash
#!/bin/bash
# Compare sRPC vs RDMA latency

echo "=== sRPC (TCP) Latency ==="
./build/rpcbench -s 0.0.0.0:8848 -f &
sleep 2
./build/rpcbench -c localhost:8848 -f -n 30 -o 1 -t 1
pkill rpcbench

echo ""
echo "=== RDMA Latency (after implementation) ==="
# Run same test with RDMA-enabled sRPC
MAKO_TRANSPORT=srpc_rdma ./build/rpcbench -s 0.0.0.0:8848 -f &
sleep 2
MAKO_TRANSPORT=srpc_rdma ./build/rpcbench -c localhost:8848 -f -n 30 -o 1 -t 1
pkill rpcbench
```

### Benchmark 2: CPU Efficiency
```bash
#!/bin/bash
# Measure CPU usage at different throughput levels

for threads in 4 8 16; do
  echo "=== Testing with $threads threads ==="
  
  # Start server and get PID
  ./build/rpcbench -s 0.0.0.0:8848 -w $threads &
  SERVER_PID=$!
  sleep 2
  
  # Monitor CPU in background
  top -b -d 1 -p $SERVER_PID -n 30 | grep rpcbench > cpu_${threads}threads.log &
  
  # Run client
  ./build/rpcbench -c localhost:8848 -t $threads -o 5000 -n 30
  
  # Stop server
  kill $SERVER_PID
  sleep 2
done

# Analyze results
echo "CPU Usage Summary:"
for log in cpu_*threads.log; do
  echo "$log: $(awk '{sum+=$9; count++} END {print sum/count "%"}' $log)"
done
```

### Benchmark 3: Throughput Scaling
```bash
#!/bin/bash
# Test throughput scaling with thread count

echo "Threads,RPS,Latency_us,CPU_percent" > throughput_scaling.csv

for threads in 1 2 4 8 16 24; do
  ./build/rpcbench -s 0.0.0.0:8848 -w $threads &
  SERVER_PID=$!
  sleep 2
  
  # Run benchmark and capture output
  output=$(./build/rpcbench -c localhost:8848 -t $threads -o 5000 -n 30)
  rps=$(echo "$output" | grep "Average:" | awk '{print $2}')
  
  # Get CPU usage
  cpu=$(top -b -n 1 -p $SERVER_PID | grep rpcbench | awk '{print $9}')
  
  echo "$threads,$rps,TODO,$cpu" >> throughput_scaling.csv
  
  kill $SERVER_PID
  sleep 2
done

cat throughput_scaling.csv
```

---

## 📈 Success Criteria

### Minimum Viable RDMA Implementation:
- ✅ **Latency:** < 5 μs average (vs 15-30 μs in sRPC)
- ✅ **CPU:** < 50% of sRPC CPU usage at same RPS
- ✅ **Throughput:** > 300K RPS (vs 100-200K in sRPC)

### Excellent RDMA Implementation:
- ✅ **Latency:** < 2 μs average
- ✅ **CPU:** < 30% of sRPC CPU usage
- ✅ **Throughput:** > 500K RPS
- ✅ **Scalability:** Linear scaling up to 16+ threads

---

## 🔍 What NOT to Focus On (For Now)

❌ **Geo-distributed testing** - Not relevant for RDMA benefits  
❌ **Multi-datacenter latency** - RDMA is for single-datacenter  
❌ **10-shard deployments** - Focus on single-node RPC first  
❌ **TPC-C throughput** - Use simple RPC benchmarks instead  
❌ **Replication performance** - Test basic RPC first  

---

## 🛠️ Recommended Testing Setup

### Hardware Requirements:
- **Minimum:** Single machine with RDMA NIC (Mellanox ConnectX-4/5)
- **Ideal:** 2 machines with RDMA NICs connected directly or via RDMA switch
- **Alternative:** Use SoftRoCE (software RDMA) for development

### Software Requirements:
```bash
# Install RDMA drivers
sudo apt-get install libibverbs-dev librdmacm-dev

# Verify RDMA devices
ibstat
ibv_devices

# Configure huge pages (required for eRPC/RDMA)
echo 2048 | sudo tee /proc/sys/vm/nr_hugepages
```

### Test Environment:
```bash
# Use localhost for initial testing (loopback RDMA)
# Then test with actual RDMA NICs for real performance

# Check RDMA is working
rping -s &  # Server
rping -c -a <server-ip>  # Client
```

---

## 📊 Metrics Collection Script

Here's a complete script to collect all relevant metrics:

```bash
#!/bin/bash
# rdma_metrics_collector.sh

RESULTS_DIR="rdma_metrics_$(date +%Y%m%d_%H%M%S)"
mkdir -p $RESULTS_DIR

echo "========================================="
echo "RDMA Implementation Metrics Collection"
echo "========================================="

# 1. Latency Test
echo "1. Testing RPC Latency..."
./build/rpcbench -s 0.0.0.0:8848 -f &
SERVER_PID=$!
sleep 2
./build/rpcbench -c localhost:8848 -f -n 30 -o 1 -t 1 > $RESULTS_DIR/latency.txt
kill $SERVER_PID
sleep 2

# 2. CPU Efficiency Test
echo "2. Testing CPU Efficiency..."
./build/rpcbench -s 0.0.0.0:8848 -w 8 &
SERVER_PID=$!
sleep 2
top -b -d 1 -p $SERVER_PID -n 30 > $RESULTS_DIR/cpu_usage.txt &
perf stat -p $SERVER_PID -o $RESULTS_DIR/perf_stats.txt sleep 30 &
./build/rpcbench -c localhost:8848 -t 8 -o 5000 -n 30 > $RESULTS_DIR/throughput.txt
kill $SERVER_PID
sleep 2

# 3. Message Size Scaling
echo "3. Testing Message Size Scaling..."
./build/rpcbench -s 0.0.0.0:8848 &
SERVER_PID=$!
sleep 2
echo "Size,RPS,Latency" > $RESULTS_DIR/message_size_scaling.csv
for size in 10 100 1024 4096 8192; do
  output=$(./build/rpcbench -c localhost:8848 -b $size -n 10)
  rps=$(echo "$output" | grep "Average:" | awk '{print $2}')
  echo "$size,$rps,TODO" >> $RESULTS_DIR/message_size_scaling.csv
done
kill $SERVER_PID

# 4. Thread Scaling
echo "4. Testing Thread Scaling..."
echo "Threads,RPS" > $RESULTS_DIR/thread_scaling.csv
for threads in 1 2 4 8 16; do
  ./build/rpcbench -s 0.0.0.0:8848 -w $threads &
  SERVER_PID=$!
  sleep 2
  output=$(./build/rpcbench -c localhost:8848 -t $threads -o 5000 -n 20)
  rps=$(echo "$output" | grep "Average:" | awk '{print $2}')
  echo "$threads,$rps" >> $RESULTS_DIR/thread_scaling.csv
  kill $SERVER_PID
  sleep 2
done

echo ""
echo "========================================="
echo "Results saved to: $RESULTS_DIR"
echo "========================================="
echo ""
echo "Key Metrics:"
echo "- Latency: $(grep "Average:" $RESULTS_DIR/latency.txt)"
echo "- Throughput: $(grep "Average:" $RESULTS_DIR/throughput.txt)"
echo "- CPU Usage: $(grep rpcbench $RESULTS_DIR/cpu_usage.txt | awk '{sum+=$9; count++} END {print sum/count "%"}')"
```

---

## 📝 Summary

### Focus on These 3 Core Metrics:

1. **RPC Latency** (μs) - Target: < 2 μs (vs 15-30 μs)
2. **CPU Utilization** (%) - Target: 50% reduction at same RPS
3. **Throughput** (RPS) - Target: > 500K RPS (vs 100-200K)

### Use These Tools:

- **`rpcbench`** - Primary RPC benchmarking tool
- **`perf stat`** - CPU cycle analysis
- **`top`/`htop`** - CPU utilization monitoring
- **`vmstat`** - Context switch tracking

### Skip These (For Now):

- Geo-distributed testing
- Multi-shard TPC-C benchmarks
- Replication performance
- Cross-datacenter latency

### Your Testing Flow:

1. **Baseline:** Measure current sRPC performance with `rpcbench`
2. **Implement:** Add RDMA to sRPC (from eRPC code)
3. **Validate:** Run same `rpcbench` tests, compare metrics
4. **Optimize:** Iterate on CPU usage and latency
5. **Scale:** Test thread scaling and message sizes

**Goal:** Prove RDMA in sRPC gives 5-10x latency improvement and 50% CPU reduction! 🚀
