#!/bin/bash

# Script to test 1-shard experiments with replication
# Each shard should:
# 1. Show "agg_persist_throughput" keyword
# 2. Have NewOrder_remote_abort_ratio < 20%
# 3. Followers replay at least 1000 batches

echo "========================================="
echo "Testing 1-shard setup with replication"
echo "========================================="

trd=${1:-6}
script_name="$(basename "$0")"
ps aux | grep -i dbtest | awk "{print \$2}" | xargs kill -9 2>/dev/null
# Clean up old log files
rm -f nfs_sync_*
USERNAME=${USER:-unknown}
rm -rf /tmp/${USERNAME}_mako_rocksdb_shard*

# Start shard 0 in background
echo "Starting shard 0..."
nohup bash bash/shard.sh 1 0 $trd localhost 0 1 > $script_name\_shard0-localhost-$trd.log 2>&1 &
nohup bash bash/shard.sh 1 0 $trd learner 0 1 > $script_name\_shard0-learner-$trd.log 2>&1 &
nohup bash bash/shard.sh 1 0 $trd p2 0 1 > $script_name\_shard0-p2-$trd.log 2>&1 &
sleep 1
nohup bash bash/shard.sh 1 0 $trd p1 0 1 > $script_name\_shard0-p1-$trd.log 2>&1 &
SHARD0_PID=$!
sleep 2

# Wait for experiments to run
echo "Running experiments for 30 seconds..."
sleep 60

# Kill the processes
echo "Stopping shards..."
# Kill all dbtest processes (all 4 instances with different cluster names)
pkill -9 -f "dbtest.*shard-index 0" 2>/dev/null || true
sleep 2
# Original cleanup for good measure
kill $SHARD0_PID 2>/dev/null || true
wait $SHARD0_PID 2>/dev/null || true

echo ""
echo "========================================="
echo "Checking test results..."
echo "========================================="

failed=0

# Check each shard's output
{
    i=0
    log="${script_name}_shard${i}-localhost-$trd.log"
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
        grep "agg_persist_throughput" "$log" | tail -1 | sed 's/^/    /'
    else
        echo "  ✗ 'agg_persist_throughput' keyword not found"
        failed=1
    fi
    
    # Check NewOrder_remote_abort_ratio
    if grep -q "NewOrder_remote_abort_ratio:" "$log"; then
        # Extract the abort ratio value
        abort_ratio=$(grep "NewOrder_remote_abort_ratio:" "$log" | tail -1 | awk '{print $2}')
        
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
}

# Check replay_batch counter in shard0-p1.log
echo ""
log_p1="${script_name}_shard0-p1-$trd.log"
echo "Checking $log_p1:"
echo "-----------------"

if [ ! -f "$log_p1" ]; then
    echo "  ✗ $log_p1 file not found"
    failed=1
else
    # Get the last occurrence of replay_batch
    last_replay_batch=$(grep "replay_batch:" "$log_p1" | tail -1)
    
    if [ -z "$last_replay_batch" ]; then
        echo "  ✗ No 'replay_batch' keyword found in $log_p1"
        failed=1
    else
        # Extract the replay_batch number (assuming format: "replay_batch:XXX")
        replay_count=$(echo "$last_replay_batch" | sed -n 's/.*replay_batch:\([0-9]*\).*/\1/p')
        
        if [ -z "$replay_count" ]; then
            echo "  ✗ Could not extract replay_batch value"
            echo "    Last line: $last_replay_batch"
            failed=1
        else
            # Check if replay_count is greater than 1000
            if [ "$replay_count" -gt 1000 ]; then
                echo "  ✓ replay_batch: $replay_count (> 1000)"
            else
                echo "  ✗ replay_batch: $replay_count (should be > 1000)"
                failed=1
            fi
        fi
    fi
fi

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
    echo "Check $script_name\_shard0-localhost-$trd.log and $log_p1 for details"
    echo ""
    echo "Last 10 lines of $script_name\_shard0-localhost-$trd.log:"
    tail -10 $script_name\_shard0-localhost-$trd.log 
    echo ""
    echo "Last 5 lines with 'replay_batch' from $log_p1:"
    grep "replay_batch" $log_p1 | tail -5 2>/dev/null || echo "No replay_batch entries found"
    exit 1
fi
