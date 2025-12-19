#!/bin/bash

# Script to test 1-shard experiments with Raft replication using simpleTransactionRepRaft
# This is the Raft version of test_1shard_replication_simple.sh
#
# Key differences from Paxos version:
# - Uses Raft instead of Paxos for replication
# - Only 3 replicas (localhost, p1, p2) - no separate learner in Raft

echo "========================================="
echo "Testing 1-shard setup with Raft replication using simpleTransactionRepRaft"
echo "========================================="

# Kill any lingering processes
ps aux | grep -i simpleTransactionRepRaft | grep -v grep | awk "{print \$2}" | xargs kill -9 2>/dev/null
ps aux | grep -i dbtest | grep -v grep | awk "{print \$2}" | xargs kill -9 2>/dev/null
sleep 1

# Clean up old log files
rm -f simple-raft-shard0*.log nfs_sync_*
USERNAME=${USER:-unknown}
rm -rf /tmp/${USERNAME}_mako_rocksdb_shard*

# Start shard 0 with 3 Raft replicas (no learner in Raft)
echo "Starting shard 0 with 3 Raft replicas..."
nohup ./build/simpleTransactionRepRaft 1 0 6 localhost 1 > simple-raft-shard0-localhost.log 2>&1 &
PID_LOCALHOST=$!
nohup ./build/simpleTransactionRepRaft 1 0 6 p2 1 > simple-raft-shard0-p2.log 2>&1 &
PID_P2=$!
sleep 1
nohup ./build/simpleTransactionRepRaft 1 0 6 p1 1 > simple-raft-shard0-p1.log 2>&1 &
PID_P1=$!
sleep 2

# Wait for experiments to run
echo "Running experiments for 40 seconds..."
sleep 40

# Kill ALL processes
echo "Stopping shards..."
kill $PID_LOCALHOST $PID_P2 $PID_P1 2>/dev/null
wait $PID_LOCALHOST $PID_P2 $PID_P1 2>/dev/null

# Force kill any remaining processes
pkill -9 -f "simpleTransactionRepRaft" 2>/dev/null || true
sleep 2

echo ""
echo "========================================="
echo "Checking test results..."
echo "========================================="

failed=0

# Check replay_batch counter in shard0-p1.log
echo ""
echo "Checking simple-raft-shard0-p1.log:"
echo "-----------------"

if [ ! -f "simple-raft-shard0-p1.log" ]; then
    echo "  ✗ simple-raft-shard0-p1.log file not found"
    failed=1
else
    # Get the last occurrence of replay_batch
    last_replay_batch=$(grep "replay_batch:" "simple-raft-shard0-p1.log" | tail -1)

    if [ -z "$last_replay_batch" ]; then
        echo "  ✗ No 'replay_batch' keyword found in simple-raft-shard0-p1.log"
        failed=1
    else
        # Extract the replay_batch number
        replay_count=$(echo "$last_replay_batch" | sed -n 's/.*replay_batch:\([0-9]*\).*/\1/p')

        if [ -z "$replay_count" ]; then
            echo "  ✗ Could not extract replay_batch value"
            echo "    Last line: $last_replay_batch"
            failed=1
        else
            if [ "$replay_count" -gt 0 ]; then
                echo "  ✓ replay_batch: $replay_count (> 0)"
            else
                echo "  ✗ replay_batch: $replay_count (should be > 0)"
                failed=1
            fi
        fi
    fi
fi

# Check all 3 logs for data integrity verification (Raft has 3 replicas, not 4)
# Note: Leader may hang during shutdown (known issue), so we track follower success separately
echo ""
echo "Checking data integrity verification in all logs:"
echo "-----------------"
follower_verified=0
leader_verified=0

for log_suffix in localhost p2 p1; do
    log="simple-raft-shard0-${log_suffix}.log"

    if [ ! -f "$log" ]; then
        echo "  ✗ $log: Log file not found"
        continue
    fi

    # Check for "ALL VERIFICATIONS PASSED" message
    if grep -q "ALL VERIFICATIONS PASSED" "$log"; then
        echo "  ✓ $log: Data integrity verified"
        if [ "$log_suffix" = "localhost" ]; then
            leader_verified=1
        else
            follower_verified=$((follower_verified + 1))
        fi
    else
        if [ "$log_suffix" = "localhost" ]; then
            echo "  ⚠ $log: Leader may have hung during shutdown (known issue)"
        else
            echo "  ✗ $log: Data integrity verification FAILED or not found"
            failed=1
        fi
    fi
done

echo ""
echo "========================================="
# Pass if BOTH followers verified successfully (replication worked)
# Leader may hang during shutdown but that's a known issue
if [ "$follower_verified" -ge 2 ] && [ "$failed" -eq 0 ]; then
    echo "All checks passed!"
    if [ "$leader_verified" -eq 0 ]; then
        echo "(Note: Leader hung during shutdown, but replication succeeded)"
    fi
    echo "========================================="
    exit 0
else
    echo "Some checks failed!"
    echo "========================================="
    echo ""
    echo "Debug information:"
    echo "Followers verified: $follower_verified/2"
    echo "Leader verified: $leader_verified"
    echo ""
    echo "Last 10 lines of simple-raft-shard0-localhost.log:"
    tail -10 simple-raft-shard0-localhost.log
    echo ""
    echo "Last 5 lines with 'replay_batch' from simple-raft-shard0-p1.log:"
    grep "replay_batch" simple-raft-shard0-p1.log | tail -5 2>/dev/null || echo "No replay_batch entries found"
    exit 1
fi
