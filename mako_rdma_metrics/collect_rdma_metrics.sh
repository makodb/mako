#!/bin/bash
# RDMA Implementation Metrics Collection Script
# Focused on: Latency, CPU Usage, and Throughput

set -e

RESULTS_DIR="rdma_metrics_$(date +%Y%m%d_%H%M%S)"
mkdir -p $RESULTS_DIR

echo "========================================="
echo "RDMA Implementation Metrics Collection"
echo "========================================="
echo ""
echo "This will measure:"
echo "  1. RPC Latency (target: < 2 μs)"
echo "  2. CPU Utilization (target: 50% reduction)"
echo "  3. Throughput (target: > 500K RPS)"
echo ""
echo "Results will be saved to: $RESULTS_DIR"
echo ""

# Cleanup function
cleanup() {
    pkill -9 rpcbench 2>/dev/null || true
    sleep 1
}

trap cleanup EXIT

# 1. LATENCY TEST (Most Important)
echo "========================================="
echo "1. Testing RPC Latency (Low Concurrency)"
echo "========================================="
cleanup

./build/rpcbench -s 0.0.0.0:8848 -f > /dev/null 2>&1 &
SERVER_PID=$!
sleep 2

echo "Running latency benchmark (30 seconds)..."
./build/rpcbench -c localhost:8848 -f -n 30 -o 1 -t 1 | tee $RESULTS_DIR/latency_test.log

cleanup
echo ""

# 2. CPU EFFICIENCY TEST
echo "========================================="
echo "2. Testing CPU Efficiency at High Load"
echo "========================================="
cleanup

./build/rpcbench -s 0.0.0.0:8848 -w 8 > /dev/null 2>&1 &
SERVER_PID=$!
sleep 2

echo "Monitoring CPU usage (30 seconds)..."
# Monitor CPU in background
top -b -d 1 -p $SERVER_PID -n 30 | grep rpcbench > $RESULTS_DIR/cpu_usage_raw.txt &
TOP_PID=$!

# Run perf if available
if command -v perf &> /dev/null; then
    perf stat -p $SERVER_PID -e cycles,instructions,cache-misses -o $RESULTS_DIR/perf_stats.txt sleep 30 &
fi

# Run client benchmark
./build/rpcbench -c localhost:8848 -t 8 -o 5000 -n 30 | tee $RESULTS_DIR/throughput_test.log

wait $TOP_PID 2>/dev/null || true
cleanup
echo ""

# 3. THROUGHPUT SCALING TEST
echo "========================================="
echo "3. Testing Throughput Scaling (Threads)"
echo "========================================="
echo "Threads,RPS,Avg_Latency_us" > $RESULTS_DIR/thread_scaling.csv

for threads in 1 2 4 8 16; do
    cleanup
    echo "Testing with $threads threads..."
    
    ./build/rpcbench -s 0.0.0.0:8848 -w $threads > /dev/null 2>&1 &
    SERVER_PID=$!
    sleep 2
    
    output=$(./build/rpcbench -c localhost:8848 -t $threads -o 5000 -n 20 2>&1)
    rps=$(echo "$output" | grep "Average:" | awk '{print $2}' || echo "0")
    
    echo "$threads,$rps,TODO" >> $RESULTS_DIR/thread_scaling.csv
    echo "  $threads threads: $rps RPS"
done

cleanup
echo ""

# 4. MESSAGE SIZE SCALING TEST
echo "========================================="
echo "4. Testing Message Size Impact"
echo "========================================="
cleanup

./build/rpcbench -s 0.0.0.0:8848 > /dev/null 2>&1 &
SERVER_PID=$!
sleep 2

echo "MsgSize_bytes,RPS" > $RESULTS_DIR/message_size_scaling.csv

for size in 10 100 1024 4096 8192; do
    echo "Testing message size: $size bytes..."
    output=$(./build/rpcbench -c localhost:8848 -b $size -n 10 2>&1)
    rps=$(echo "$output" | grep "Average:" | awk '{print $2}' || echo "0")
    echo "$size,$rps" >> $RESULTS_DIR/message_size_scaling.csv
    echo "  $size bytes: $rps RPS"
done

cleanup
echo ""

# 5. ANALYZE RESULTS
echo "========================================="
echo "RESULTS SUMMARY"
echo "========================================="
echo ""

# Latency
echo "=== LATENCY ==="
if [ -f "$RESULTS_DIR/latency_test.log" ]; then
    avg_rps=$(grep "Average:" $RESULTS_DIR/latency_test.log | awk '{print $2}')
    echo "Average RPS (low concurrency): $avg_rps"
    echo "Estimated latency: ~$((1000000 / avg_rps)) μs (1M / RPS)"
else
    echo "No latency data found"
fi
echo ""

# CPU Usage
echo "=== CPU UTILIZATION ==="
if [ -f "$RESULTS_DIR/cpu_usage_raw.txt" ]; then
    avg_cpu=$(awk '{sum+=$9; count++} END {if(count>0) print sum/count; else print "N/A"}' $RESULTS_DIR/cpu_usage_raw.txt)
    echo "Average CPU usage: ${avg_cpu}%"
else
    echo "No CPU data found"
fi
echo ""

# Throughput
echo "=== THROUGHPUT ==="
if [ -f "$RESULTS_DIR/throughput_test.log" ]; then
    max_rps=$(grep "Average:" $RESULTS_DIR/throughput_test.log | awk '{print $2}')
    echo "Max throughput (8 threads): $max_rps RPS"
else
    echo "No throughput data found"
fi
echo ""

# Thread Scaling
echo "=== THREAD SCALING ==="
if [ -f "$RESULTS_DIR/thread_scaling.csv" ]; then
    cat $RESULTS_DIR/thread_scaling.csv
else
    echo "No scaling data found"
fi
echo ""

# Message Size
echo "=== MESSAGE SIZE IMPACT ==="
if [ -f "$RESULTS_DIR/message_size_scaling.csv" ]; then
    cat $RESULTS_DIR/message_size_scaling.csv
else
    echo "No message size data found"
fi
echo ""

# Perf Stats
if [ -f "$RESULTS_DIR/perf_stats.txt" ]; then
    echo "=== PERF STATISTICS ==="
    cat $RESULTS_DIR/perf_stats.txt
    echo ""
fi

echo "========================================="
echo "All results saved to: $RESULTS_DIR"
echo "========================================="
echo ""
echo "Key Files:"
echo "  - latency_test.log          : Latency benchmark output"
echo "  - throughput_test.log       : Throughput benchmark output"
echo "  - cpu_usage_raw.txt         : CPU usage over time"
echo "  - thread_scaling.csv        : Throughput vs thread count"
echo "  - message_size_scaling.csv  : Throughput vs message size"
if [ -f "$RESULTS_DIR/perf_stats.txt" ]; then
    echo "  - perf_stats.txt            : CPU cycle analysis"
fi
echo ""

# Create summary file
cat > $RESULTS_DIR/SUMMARY.txt << EOF
RDMA Implementation Metrics Summary
Generated: $(date)
========================================

TARGET METRICS (for RDMA implementation):
- Latency: < 2 μs (vs 15-30 μs in sRPC/TCP)
- CPU Usage: 50% reduction at same RPS
- Throughput: > 500K RPS (vs 100-200K in sRPC/TCP)

CURRENT RESULTS:
EOF

if [ -f "$RESULTS_DIR/latency_test.log" ]; then
    avg_rps=$(grep "Average:" $RESULTS_DIR/latency_test.log | awk '{print $2}')
    echo "- Latency: ~$((1000000 / avg_rps)) μs" >> $RESULTS_DIR/SUMMARY.txt
fi

if [ -f "$RESULTS_DIR/cpu_usage_raw.txt" ]; then
    avg_cpu=$(awk '{sum+=$9; count++} END {if(count>0) print sum/count; else print "N/A"}' $RESULTS_DIR/cpu_usage_raw.txt)
    echo "- CPU Usage: ${avg_cpu}%" >> $RESULTS_DIR/SUMMARY.txt
fi

if [ -f "$RESULTS_DIR/throughput_test.log" ]; then
    max_rps=$(grep "Average:" $RESULTS_DIR/throughput_test.log | awk '{print $2}')
    echo "- Max Throughput: $max_rps RPS" >> $RESULTS_DIR/SUMMARY.txt
fi

echo "" >> $RESULTS_DIR/SUMMARY.txt
echo "See individual log files for detailed results." >> $RESULTS_DIR/SUMMARY.txt

cat $RESULTS_DIR/SUMMARY.txt

echo ""
echo "✅ Metrics collection complete!"
