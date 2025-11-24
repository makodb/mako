#!/bin/bash

# Mako Metrics Extraction Script
# Usage: ./extract_metrics.sh <logfile>
# Example: ./extract_metrics.sh test_1shard_replication.sh_shard0-localhost-6.log

if [ $# -eq 0 ]; then
    echo "Usage: $0 <logfile>"
    echo "Example: $0 test_1shard_replication.sh_shard0-localhost-6.log"
    exit 1
fi

LOGFILE=$1

if [ ! -f "$LOGFILE" ]; then
    echo "Error: Log file '$LOGFILE' not found"
    exit 1
fi

echo "========================================"
echo "Mako Benchmark Metrics Extraction"
echo "Log file: $LOGFILE"
echo "========================================"
echo ""

# Check if benchmark completed
if ! grep -q "benchmark statistics" "$LOGFILE"; then
    echo "Warning: Benchmark statistics not found in log file"
    echo "The benchmark may not have completed successfully"
    exit 1
fi

echo "=== CORE PERFORMANCE METRICS ==="
echo ""

# Runtime
runtime=$(grep "^runtime:" "$LOGFILE" | tail -1 | awk '{print $2, $3}')
echo "Runtime: $runtime"

# Total commits
n_commits=$(grep "^n_commits:" "$LOGFILE" | tail -1 | awk '{print $2}')
echo "Total Commits: $n_commits"

# Throughput
agg_throughput=$(grep "^agg_throughput:" "$LOGFILE" | tail -1 | awk '{print $2, $3}')
echo "Aggregate Throughput: $agg_throughput"

avg_per_core=$(grep "^avg_per_core_throughput:" "$LOGFILE" | tail -1 | awk '{print $2, $3}')
echo "Avg Per-Core Throughput: $avg_per_core"

# Persist throughput
persist_throughput=$(grep "^agg_persist_throughput:" "$LOGFILE" | tail -1 | awk '{print $2, $3}')
echo "Persist Throughput: $persist_throughput"

echo ""
echo "=== LATENCY METRICS ==="
echo ""

# Average latency
avg_latency=$(grep "^avg_latency:" "$LOGFILE" | tail -1 | awk '{print $2, $3}')
echo "Average Latency: $avg_latency"

# Persist latency
persist_latency=$(grep "^avg_persist_latency:" "$LOGFILE" | tail -1 | awk '{print $2, $3}')
echo "Persist Latency: $persist_latency"

echo ""
echo "=== ABORT METRICS ==="
echo ""

# Abort rate
abort_rate=$(grep "^agg_abort_rate:" "$LOGFILE" | tail -1 | awk '{print $2, $3}')
echo "Aggregate Abort Rate: $abort_rate"

avg_abort=$(grep "^avg_per_core_abort_rate:" "$LOGFILE" | tail -1 | awk '{print $2, $3}')
echo "Avg Per-Core Abort Rate: $avg_abort"

echo ""
echo "=== TPC-C TRANSACTION METRICS ==="
echo ""

# NewOrder metrics
if grep -q "NewOrder_local_commit_latency:" "$LOGFILE"; then
    echo "--- NewOrder Transaction ---"
    grep "NewOrder_local_commit_latency:" "$LOGFILE" | tail -1 | sed 's/^/  /'
    grep "NewOrder_local_abort_ratio:" "$LOGFILE" | tail -1 | sed 's/^/  /'
    
    if grep -q "NewOrder_remote_ratio:" "$LOGFILE"; then
        grep "NewOrder_remote_ratio:" "$LOGFILE" | tail -1 | sed 's/^/  /'
        grep "NewOrder_remote_abort_ratio:" "$LOGFILE" | tail -1 | sed 's/^/  /'
        grep "NewOrder_remote_commit_latency:" "$LOGFILE" | tail -1 | sed 's/^/  /'
    fi
    echo ""
fi

# Payment metrics
if grep -q "Payment_local_commit_latency:" "$LOGFILE"; then
    echo "--- Payment Transaction ---"
    grep "Payment_local_commit_latency:" "$LOGFILE" | tail -1 | sed 's/^/  /'
    grep "Payment_local_abort_ratio:" "$LOGFILE" | tail -1 | sed 's/^/  /'
    
    if grep -q "Payment_remote_ratio:" "$LOGFILE"; then
        grep "Payment_remote_ratio:" "$LOGFILE" | tail -1 | sed 's/^/  /'
        grep "Payment_remote_abort_ratio:" "$LOGFILE" | tail -1 | sed 's/^/  /'
    fi
    echo ""
fi

# Delivery metrics
if grep -q "Delivery_local_commit_latency:" "$LOGFILE"; then
    echo "--- Delivery Transaction ---"
    grep "Delivery_local_commit_latency:" "$LOGFILE" | tail -1 | sed 's/^/  /'
    echo ""
fi

# OrderStatus metrics
if grep -q "OrderStatus_local_commit_latency:" "$LOGFILE"; then
    echo "--- OrderStatus Transaction ---"
    grep "OrderStatus_local_commit_latency:" "$LOGFILE" | tail -1 | sed 's/^/  /'
    echo ""
fi

# StockLevel metrics
if grep -q "StockLevel_local_commit_latency:" "$LOGFILE"; then
    echo "--- StockLevel Transaction ---"
    grep "StockLevel_local_commit_latency:" "$LOGFILE" | tail -1 | sed 's/^/  /'
    echo ""
fi

echo "========================================"
echo "Extraction Complete"
echo "========================================"
