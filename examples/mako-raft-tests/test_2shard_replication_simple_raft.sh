#!/bin/bash

# Script to test 2-shard experiments with Raft replication using simpleTransactionRepRaft
#
# NOTE: This script mirrors test_2shard_replication_simple.sh (Paxos) exactly
# in duration, startup order, shutdown, and validation — only the replication
# layer differs (Raft 3 replicas vs Paxos 4 replicas with learner).

# Source common utilities (includes GDB_PREFIX for debugging)
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
source "${SCRIPT_DIR}/../../bash/util.sh"

echo "========================================="
echo "Testing 2-shard setup with Raft replication using simpleTransactionRepRaft"
echo "========================================="

if [ "$GDB_ENABLED" == "1" ]; then
    echo "[GDB] Debug mode enabled"
fi

# Clean up old log files
rm -f nfs_sync_*
rm -f simple-raft-shard0*.log simple-raft-shard1*.log
USERNAME=${USER:-unknown}
rm -rf /tmp/${USERNAME}_mako_rocksdb_shard*

ps aux | grep -i dbtest | awk "{print \$2}" | xargs kill -9 2>/dev/null
ps aux | grep -i simpleTransactionRepRaft | awk "{print \$2}" | xargs kill -9 2>/dev/null
sleep 1

trd=6

# Start BOTH shards simultaneously to avoid timing issues where shard 0 tries
# to connect to shard 1 before shard 1 is ready (same strategy as Paxos)
echo "Starting shard 0 and shard 1 simultaneously..."

# Start shard 0 followers first (no learner in Raft)
nohup $GDB_PREFIX ./${BUILD_DIR:-build}/simpleTransactionRepRaft 2 0 $trd p2 1 > simple-raft-shard0-p2.log 2>&1 &
PID_S0_P2=$!

# Start shard 1 followers simultaneously
nohup $GDB_PREFIX ./${BUILD_DIR:-build}/simpleTransactionRepRaft 2 1 $trd p2 1 > simple-raft-shard1-p2.log 2>&1 &
PID_S1_P2=$!

sleep 2

# Start p1 followers
nohup $GDB_PREFIX ./${BUILD_DIR:-build}/simpleTransactionRepRaft 2 0 $trd p1 1 > simple-raft-shard0-p1.log 2>&1 &
PID_S0_P1=$!
nohup $GDB_PREFIX ./${BUILD_DIR:-build}/simpleTransactionRepRaft 2 1 $trd p1 1 > simple-raft-shard1-p1.log 2>&1 &
PID_S1_P1=$!

sleep 3

# Start leaders simultaneously — they wait 5s for setup before starting tests
nohup $GDB_PREFIX ./${BUILD_DIR:-build}/simpleTransactionRepRaft 2 0 $trd localhost 1 > simple-raft-shard0-localhost.log 2>&1 &
PID_S0_LOCALHOST=$!
nohup $GDB_PREFIX ./${BUILD_DIR:-build}/simpleTransactionRepRaft 2 1 $trd localhost 1 > simple-raft-shard1-localhost.log 2>&1 &
PID_S1_LOCALHOST=$!

# Wait for experiments to run (same duration as Paxos: 90s)
# Includes 5s setup wait + verification in simpleTransactionRepRaft
echo "Running experiments for 90 seconds..."
sleep 90

# Kill ALL processes from both shards (same as Paxos)
echo "Stopping shards..."
kill $PID_S0_LOCALHOST $PID_S0_P2 $PID_S0_P1 \
     $PID_S1_LOCALHOST $PID_S1_P2 $PID_S1_P1 2>/dev/null
wait $PID_S0_LOCALHOST $PID_S0_P2 $PID_S0_P1 \
     $PID_S1_LOCALHOST $PID_S1_P2 $PID_S1_P1 2>/dev/null

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
        # Extract the replay_batch number (assuming format: "replay_batch:XXX")
        replay_count=$(echo "$last_replay_batch" | sed -n 's/.*replay_batch:\([0-9]*\).*/\1/p')

        if [ -z "$replay_count" ]; then
            echo "  ✗ Could not extract replay_batch value"
            echo "    Last line: $last_replay_batch"
            failed=1
        else
            # Check if replay_count is greater than 0 (same threshold as Paxos)
            if [ "$replay_count" -gt 0 ]; then
                echo "  ✓ replay_batch: $replay_count (> 0)"
            else
                echo "  ✗ replay_batch: $replay_count (should be > 0)"
                failed=1
            fi
        fi
    fi

done

# Check follower logs for data integrity verification (same as Paxos, minus learner)
# Note: Leaders (localhost) are the source of data and may have cleanup issues,
# so we only verify followers (p1, p2) which receive replicated data
echo ""
echo "Checking data integrity verification in follower logs:"
echo "-----------------"
for shard in 0 1; do
    for log_suffix in p2 p1; do
        log="simple-raft-shard${shard}-${log_suffix}.log"

        if [ ! -f "$log" ]; then
            echo "  ✗ $log: Log file not found"
            failed=1
            continue
        fi

        # Check for "ALL VERIFICATIONS PASSED" message
        if grep -q "ALL VERIFICATIONS PASSED" "$log"; then
            echo "  ✓ $log: Data integrity verified"
        else
            echo "  ✗ $log: Data integrity verification FAILED or not found"
            failed=1
        fi
    done
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
    echo "Check simple-raft-shard0-localhost.log and simple-raft-shard1-localhost.log for details"
    tail -10 simple-raft-shard0-localhost.log
    tail -10 simple-raft-shard1-localhost.log
    exit 1
fi
