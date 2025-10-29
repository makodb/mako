#!/bin/bash

# Script to test 2-shard experiments with replication using simpleTransactionRep

echo "========================================="
echo "Testing 2-shard setup with replication using simpleTransactionRep"
echo "========================================="

#skill dbtest
# Clean up old log files
rm -f nfs_sync_*
rm -f simple-shard0*.log simple-shard1*.log
rm -rf /tmp/mako_rocksdb_shard*

ps aux | grep -i dbtest | awk "{print \$2}" | xargs kill -9 2>/dev/null
ps aux | grep -i simplePaxos | awk "{print \$2}" | xargs kill -9 2>/dev/null
ps aux | grep -i simpleTransactionRep | awk "{print \$2}" | xargs kill -9 2>/dev/null
sleep 1

# Start shard 0 in background
echo "Starting shard 0..."
trd=6
nohup ./build/simpleTransactionRep 2 0 $trd localhost 0 > simple-shard0-localhost.log 2>&1 &
SHARD0_PID=$!

# Start shard 1 in background
echo "Starting shard 1..."
nohup ./build/simpleTransactionRep 2 1 $trd localhost 0 > simple-shard1-localhost.log 2>&1 &
SHARD1_PID=$!

# Wait for experiments to run
echo "Running experiments for 30 seconds..."
sleep 30

# Kill the processes
echo "Stopping shards..."
kill $SHARD0_PID $SHARD1_PID 2>/dev/null
wait $SHARD0_PID $SHARD1_PID 2>/dev/null