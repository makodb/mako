#!/bin/bash

# Script to test 2-shard experiments without replication
# Each shard should:
# 1. Show "agg_persist_throughput" keyword
# 2. Have NewOrder_remote_abort_ratio < 20%

echo "========================================="
echo "Testing 2-shard setup without replication"
echo "========================================="

# Clean up old log files
rm -f nfs_sync_*

trd=${1:-6}
script_name="$(basename "$0")"

# Determine transport type and create unique log prefix
transport="${MAKO_TRANSPORT:-rrr}"
log_prefix="${script_name}_${transport}"

ps aux | grep -i dbtest | awk "{print \$2}" | xargs kill -9 2>/dev/null
sleep 1
# Start shard 0 in background
echo "Starting shard 0..."
nohup bash bash/shard.sh 2 0 $trd localhost > ${log_prefix}_shard0-$trd.log 2>&1 &
SHARD0_PID=$!
sleep 2

# Start shard 1 in background
echo "Starting shard 1..."
nohup bash bash/shard.sh 2 1 $trd localhost > ${log_prefix}_shard1-$trd.log 2>&1 &
SHARD1_PID=$!

# Wait for experiments to run
echo "Running experiments for 30 seconds..."
sleep 50

# Kill the processes
echo "Stopping shards..."
kill $SHARD0_PID $SHARD1_PID 2>/dev/null
wait $SHARD0_PID $SHARD1_PID 2>/dev/null

echo ""
echo "========================================="
echo "Checking test results..."
echo "========================================="

failed=0

# Check each shard's output
for i in 0 1; do
    log="${log_prefix}_shard${i}-$trd.log"
    echo ""
    echo "Checking $log:"
    echo "-----------------"
    
    if [ ! -f "$log" ]; then
        echo "  ✗ Log file not found"
        failed=1
        continue
    fi
    
    # Check for agg_persist_throughput keyword
    if grep -q "agg_persist_throughput" "$log"; then
        echo "  ✓ Found 'agg_persist_throughput' keyword"
        # Show the line for reference
        grep "agg_persist_throughput" "$log" | tail -n 1 | sed 's/^/    /'
    else
        echo "  ✗ 'agg_persist_throughput' keyword not found"
        failed=1
    fi
    
    # Check NewOrder_remote_abort_ratio
    if grep -q "NewOrder_remote_abort_ratio:" "$log"; then
        # Extract the abort ratio value
        abort_ratio=$(grep "NewOrder_remote_abort_ratio:" "$log" | tail -n 1 | awk '{print $2}')
        
        if [ -z "$abort_ratio" ]; then
            echo "  ✗ Could not extract NewOrder_remote_abort_ratio value"
            failed=1
        else
            # Remove % sign if present and convert to float
            abort_value=$(echo "$abort_ratio" | sed 's/%//')
            
            # Check if value is less than 20 using awk (more portable than bc)
            if awk "BEGIN {exit !($abort_value < 20)}"; then
                echo "  ✓ NewOrder_remote_abort_ratio: $abort_ratio (< 20%)"
            else
                echo "  ✗ NewOrder_remote_abort_ratio: $abort_ratio (>= 20%)"
                failed=1
            fi
        fi
    else
        echo "  ✗ NewOrder_remote_abort_ratio not found"
        failed=1
    fi
done

echo ""
echo "========================================="
if [ $failed -eq 0 ]; then
    echo "All checks passed!"
    echo "========================================="
    exit 0
else
    echo "Some checks failed!"
    echo "========================================="
    echo ""
    echo "Debug information:"
    echo "Check ${log_prefix}_shard*-$trd for details"
    tail -n 10 ${log_prefix}_shard0-$trd.log ${log_prefix}_shard1-$trd.log
    exit 1
fi