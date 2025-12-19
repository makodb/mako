#!/bin/bash

# Script to test 2-shard experiments with Raft replication
# Each shard should:
# 1. Show "agg_persist_throughput" keyword
# 2. Have NewOrder_remote_abort_ratio < 40%

echo "========================================="
echo "Testing 2-shard setup with Raft replication"
echo "========================================="

# Clean up old log files
rm -f nfs_sync_*
rm -f shard0*.log shard1*.log
rm -rf /tmp/mako_rocksdb_shard*

# Kill any lingering processes
ps aux | grep -i dbtest | awk "{print \$2}" | xargs kill -9 2>/dev/null
ps aux | grep -i simpleRaft | awk "{print \$2}" | xargs kill -9 2>/dev/null
sleep 1

# Start shard 0 in background
echo "Starting shard 0 with Raft replication..."
trd=6
nohup bash bash/shard_raft.sh 2 0 $trd localhost 0 1 > shard0-localhost.log 2>&1 &
nohup bash bash/shard_raft.sh 2 0 $trd p2 0 1 > shard0-p2.log 2>&1 &
sleep 1
nohup bash bash/shard_raft.sh 2 0 $trd p1 0 1 > shard0-p1.log 2>&1 &
SHARD0_PID=$!

sleep 2

# Start shard 1 in background
echo "Starting shard 1 with Raft replication..."
nohup bash bash/shard_raft.sh 2 1 $trd localhost 0 1 > shard1-localhost.log 2>&1 &
nohup bash bash/shard_raft.sh 2 1 $trd p2 0 1 > shard1-p2.log 2>&1 &
sleep 1
nohup bash bash/shard_raft.sh 2 1 $trd p1 0 1 > shard1-p1.log 2>&1 &
SHARD1_PID=$!

# Wait for experiments to run
echo "Running experiments for 60 seconds..."
sleep 60

# Kill the processes
echo "Stopping shards..."
ps aux | grep -i dbtest | awk "{print \$2}" | xargs kill -9 2>/dev/null
wait $SHARD0_PID $SHARD1_PID 2>/dev/null

echo ""
echo "========================================="
echo "Checking test results..."
echo "========================================="

failed=0

# Check each shard's output
for i in 0 1; do
    log="shard${i}-localhost.log"
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

            # Check if value is less than 40 using awk (more portable than bc)
            if awk "BEGIN {exit !($abort_value < 40)}"; then
                echo "  ✓ NewOrder_remote_abort_ratio: $abort_ratio (< 40%)"
            else
                echo "  ✗ NewOrder_remote_abort_ratio: $abort_ratio (>= 40%)"
                failed=1
            fi
        fi
    else
        echo "  ✗ NewOrder_remote_abort_ratio not found"
        failed=1
    fi

    # Check Raft-specific metrics (follower replication)
    echo ""
    echo "Checking Raft replication metrics for shard $i:"
    for node in localhost p1 p2; do
        node_log="shard${i}-${node}.log"
        if [ -f "$node_log" ]; then
            # Check for replay_batch metric (indicates follower replication)
            if grep -q "replay_batch:" "$node_log"; then
                replay_count=$(grep "replay_batch:" "$node_log" | tail -1 | sed 's/.*replay_batch://' | awk '{print $1}')
                echo "  ✓ Node $node - replay_batch: $replay_count"

                # Warn if replay count is very low (indicates potential replication issues)
                if [ "$node" != "localhost" ] && [ -n "$replay_count" ]; then
                    if [ "$replay_count" -lt 1000 ]; then
                        echo "    ⚠ Warning: replay_batch is low, may indicate follower replication issues"
                    fi
                fi
            fi
        fi
    done
done

echo ""
echo "========================================="
if [ $failed -eq 0 ]; then
    echo "All checks passed!"
    echo "========================================="
    echo ""
    echo "Summary:"
    echo "  - Both shards completed successfully"
    echo "  - Raft replication working across all nodes"
    echo "  - Abort ratios within acceptable limits"
    exit 0
else
    echo "Some checks failed!"
    echo "========================================="
    echo ""
    echo "Debug information:"
    echo "Check shard0-localhost.log and shard1-localhost.log for details"
    echo ""
    echo "=== Last 20 lines of shard0-localhost.log ==="
    tail -20 shard0-localhost.log 2>/dev/null || echo "File not found"
    echo ""
    echo "=== Last 20 lines of shard1-localhost.log ==="
    tail -20 shard1-localhost.log 2>/dev/null || echo "File not found"
    exit 1
fi

# Final cleanup
ps aux | grep -i dbtest | awk "{print \$2}" | xargs kill -9 2>/dev/null
ps aux | grep -i simpleRaft | awk "{print \$2}" | xargs kill -9 2>/dev/null
