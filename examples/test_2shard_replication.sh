#!/bin/bash

# Script to test 2-shard experiments with replication
# Each shard should:
# 1. Show "agg_persist_throughput" keyword
# 2. Have NewOrder_remote_abort_ratio < 40%

echo "========================================="
echo "Testing 2-shard setup with replication"
echo "========================================="

#skill dbtest
# Clean up old log files
#rm -f shard0*.log shard1*.log
rm -f nfs_sync_*
rm -f simple-shard0*.log simple-shard1*.log
USERNAME=${USER:-unknown}
rm -rf /tmp/${USERNAME}_mako_rocksdb_shard*

trd=6
script_name="$(basename "$0")"

# Determine transport type and create unique log prefix
transport="${MAKO_TRANSPORT:-rrr}"
log_prefix="${script_name}_${transport}"

ps aux | grep -i dbtest | awk "{print \$2}" | xargs kill -9 2>/dev/null
sleep 1
# Start shard 0 in background
echo "Starting shard 0..."
nohup bash bash/shard.sh 2 0 $trd localhost 0 1 > ${log_prefix}_shard0-localhost.log 2>&1 &
SHARD0_LOCALHOST_PID=$!
nohup bash bash/shard.sh 2 0 $trd learner 0 1 > ${log_prefix}_shard0-learner.log 2>&1 &
SHARD0_LEARNER_PID=$!
nohup bash bash/shard.sh 2 0 $trd p2 0 1 > ${log_prefix}_shard0-p2.log 2>&1 &
SHARD0_P2_PID=$!
sleep 1
nohup bash bash/shard.sh 2 0 $trd p1 0 1 > ${log_prefix}_shard0-p1.log 2>&1 &
SHARD0_P1_PID=$!

sleep 2

# Start shard 1 in background
echo "Starting shard 1..."
nohup bash bash/shard.sh 2 1 $trd localhost 0 1 > ${log_prefix}_shard1-localhost.log 2>&1 &
SHARD1_LOCALHOST_PID=$!
nohup bash bash/shard.sh 2 1 $trd learner 0 1 > ${log_prefix}_shard1-learner.log 2>&1 &
SHARD1_LEARNER_PID=$!
nohup bash bash/shard.sh 2 1 $trd p2 0 1 > ${log_prefix}_shard1-p2.log 2>&1 &
SHARD1_P2_PID=$!
sleep 1
nohup bash bash/shard.sh 2 1 $trd p1 0 1 > ${log_prefix}_shard1-p1.log 2>&1 &
SHARD1_P1_PID=$!

# Wait for experiments to run
echo "Running experiments for 30 seconds..."
sleep 70

# Kill the processes - FORCE KILL ALL
echo "Stopping shards..."

# First, kill the parent bash scripts to prevent them from respawning dbtest
pkill -9 -f "bash/shard.sh" 2>/dev/null || true

# Kill all dbtest processes immediately with SIGKILL
pkill -9 dbtest 2>/dev/null || true
killall -9 dbtest 2>/dev/null || true

# Wait for OS to clean up
sleep 2

# Check for and kill any remaining processes including zombies
remaining=$(ps aux | grep "dbtest" | grep -v grep | wc -l)
if [ "$remaining" -gt 0 ]; then
    echo "WARNING: $remaining dbtest processes still present after kill attempt"
    ps aux | grep "dbtest" | grep -v grep

    # Get PIDs and kill individually
    pids=$(ps aux | grep "dbtest" | grep -v grep | awk '{print $2}')
    for pid in $pids; do
        echo "Force killing PID $pid"
        kill -9 $pid 2>/dev/null || true
    done

    sleep 1
fi

# Final verification - reap zombie processes by explicitly waiting on child PIDs
# This ensures zombie processes are reaped by their parent (this script)
for pid in $SHARD0_LOCALHOST_PID $SHARD0_LEARNER_PID $SHARD0_P2_PID $SHARD0_P1_PID \
           $SHARD1_LOCALHOST_PID $SHARD1_LEARNER_PID $SHARD1_P2_PID $SHARD1_P1_PID; do
    wait $pid 2>/dev/null || true
done

echo ""
echo "========================================="
echo "Checking test results..."
echo "========================================="

failed=0

# Check each shard's output
for i in 0 1; do
    log="${log_prefix}_shard${i}-localhost.log"
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
    echo "Check ${log_prefix}_shard*-localhost.log for details"
    tail -10 ${log_prefix}_shard0-localhost.log
    tail -10 ${log_prefix}_shard1-localhost.log
    exit 1
fi

ps aux | grep -i dbtest | awk "{print \$2}" | xargs kill -9 2>/dev/null
