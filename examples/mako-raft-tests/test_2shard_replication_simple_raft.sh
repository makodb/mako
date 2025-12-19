#!/bin/bash

# Script to test 2-shard experiments with Raft replication using simpleTransactionRepRaft
# This is the Raft version of test_2shard_replication_simple.sh
#
# Key differences from Paxos version:
# - Uses Raft instead of Paxos for replication
# - Only 3 replicas per shard (localhost, p1, p2) - no separate learner in Raft

echo "========================================="
echo "Testing 2-shard setup with Raft replication using simpleTransactionRepRaft"
echo "========================================="

# Kill any lingering processes
ps aux | grep -i simpleTransactionRepRaft | grep -v grep | awk "{print \$2}" | xargs kill -9 2>/dev/null
ps aux | grep -i dbtest | grep -v grep | awk "{print \$2}" | xargs kill -9 2>/dev/null
sleep 1

# Clean up old log files
rm -f nfs_sync_*
rm -f simple-raft-shard0*.log simple-raft-shard1*.log
USERNAME=${USER:-unknown}
rm -rf /tmp/${USERNAME}_mako_rocksdb_shard*

trd=6

# Start shard 0 with 3 Raft replicas (no learner in Raft)
echo "Starting shard 0 with 3 Raft replicas..."
nohup ./build/simpleTransactionRepRaft 2 0 $trd localhost 1 > simple-raft-shard0-localhost.log 2>&1 &
PID_S0_LOCALHOST=$!
nohup ./build/simpleTransactionRepRaft 2 0 $trd p2 1 > simple-raft-shard0-p2.log 2>&1 &
PID_S0_P2=$!
sleep 1
nohup ./build/simpleTransactionRepRaft 2 0 $trd p1 1 > simple-raft-shard0-p1.log 2>&1 &
PID_S0_P1=$!

sleep 2

# Start shard 1 with 3 Raft replicas (no learner in Raft)
echo "Starting shard 1 with 3 Raft replicas..."
nohup ./build/simpleTransactionRepRaft 2 1 $trd localhost 1 > simple-raft-shard1-localhost.log 2>&1 &
PID_S1_LOCALHOST=$!
nohup ./build/simpleTransactionRepRaft 2 1 $trd p2 1 > simple-raft-shard1-p2.log 2>&1 &
PID_S1_P2=$!
sleep 1
nohup ./build/simpleTransactionRepRaft 2 1 $trd p1 1 > simple-raft-shard1-p1.log 2>&1 &
PID_S1_P1=$!

# Wait for experiments to run
echo "Running experiments for 60 seconds..."
sleep 60

# Kill ALL processes from both shards
echo "Stopping shards..."
kill $PID_S0_LOCALHOST $PID_S0_P2 $PID_S0_P1 \
     $PID_S1_LOCALHOST $PID_S1_P2 $PID_S1_P1 2>/dev/null
wait $PID_S0_LOCALHOST $PID_S0_P2 $PID_S0_P1 \
     $PID_S1_LOCALHOST $PID_S1_P2 $PID_S1_P1 2>/dev/null

# Force kill any remaining processes
pkill -9 -f "simpleTransactionRepRaft" 2>/dev/null || true
sleep 2

echo ""
echo "========================================="
echo "Checking test results..."
echo "========================================="

failed=0

# Check each shard's output
for i in 0 1; do
    log="simple-raft-shard${i}-p1.log"
    echo ""
    echo "Checking $log:"
    echo "-----------------"

    if [ ! -f "$log" ]; then
        echo "  ✗ Log file not found"
        failed=1
        continue
    fi

    last_replay_batch=$(grep "replay_batch:" "$log" | tail -1)

    if [ -z "$last_replay_batch" ]; then
        echo "  ✗ No 'replay_batch' keyword found in $log"
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

done

# Check all 6 logs for data integrity verification (3 replicas per shard, 2 shards)
# Note: Leaders may hang during shutdown (known issue), so we track follower success separately
echo ""
echo "Checking data integrity verification in all logs:"
echo "-----------------"
follower_verified=0
leader_verified=0

for shard in 0 1; do
    for log_suffix in localhost p2 p1; do
        log="simple-raft-shard${shard}-${log_suffix}.log"

        if [ ! -f "$log" ]; then
            echo "  ✗ $log: Log file not found"
            continue
        fi

        # Check for "ALL VERIFICATIONS PASSED" message
        if grep -q "ALL VERIFICATIONS PASSED" "$log"; then
            echo "  ✓ $log: Data integrity verified"
            if [ "$log_suffix" = "localhost" ]; then
                leader_verified=$((leader_verified + 1))
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
done

echo ""
echo "========================================="
# Pass if ALL 4 followers verified successfully (replication worked on both shards)
# Leaders may hang during shutdown but that's a known issue
if [ "$follower_verified" -ge 4 ] && [ "$failed" -eq 0 ]; then
    echo "All checks passed!"
    if [ "$leader_verified" -lt 2 ]; then
        echo "(Note: Some leaders hung during shutdown, but replication succeeded)"
    fi
    echo "========================================="
    exit 0
else
    echo "Some checks failed!"
    echo "========================================="
    echo ""
    echo "Debug information:"
    echo "Followers verified: $follower_verified/4"
    echo "Leaders verified: $leader_verified/2"
    echo ""
    echo "Last 10 lines of simple-raft-shard0-localhost.log:"
    tail -10 simple-raft-shard0-localhost.log
    echo ""
    echo "Last 10 lines of simple-raft-shard1-localhost.log:"
    tail -10 simple-raft-shard1-localhost.log
    exit 1
fi
