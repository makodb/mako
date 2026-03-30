#!/bin/bash

# Script to test 2-shard experiments without replication
# Each shard should:
# 1. Show "agg_persist_throughput" keyword
# 2. Have NewOrder_remote_abort_ratio < 20%

echo "========================================="
echo "Testing 1-shard setup without replication"
echo "========================================="

trd=${1:-6}
script_name="$(basename "$0")"

# Clean up old log files
rm -f nfs_sync_*

ps aux | grep -i dbtest | awk "{print \$2}" | xargs kill -9 2>/dev/null
sleep 1
# Start shard 0 in background
echo "Starting shard 0..."
nohup bash bash/shard.sh 1 0 $trd localhost > $script_name\_shard0-$trd.log 2>&1 &
SHARD0_PID=$!
sleep 2

# Wait for experiments to run
echo "Running experiments for 30 seconds..."
sleep 50

# Kill the processes
echo "Stopping shards..."
kill $SHARD0_PID $SHARD1_PID 2>/dev/null