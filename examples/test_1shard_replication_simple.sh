#!/bin/bash

# Script to test 1-shard experiments with replication using simpleTransactionRep

echo "========================================="
echo "Testing 1-shard setup with replication using simpleTransactionRep"
echo "========================================="

ps aux | grep -i simpleTransactionRep | awk "{print \$2}" | xargs kill -9 2>/dev/null
# Clean up old log files
rm -f simple-shard0*.log nfs_sync_*
USERNAME=${USER:-unknown}
rm -rf /tmp/${USERNAME}_mako_rocksdb_shard*

# Start shard 0 in background - capture ALL PIDs
echo "Starting shard 0..."
nohup ./build/simpleTransactionRep 1 0 6 localhost 1 > simple-shard0-localhost.log 2>&1 &
PID_LOCALHOST=$!
nohup ./build/simpleTransactionRep 1 0 6 learner 1 > simple-shard0-learner.log 2>&1 &
PID_LEARNER=$!
nohup ./build/simpleTransactionRep 1 0 6 p2 1 > simple-shard0-p2.log 2>&1 &
PID_P2=$!
sleep 1
nohup ./build/simpleTransactionRep 1 0 6 p1 1  > simple-shard0-p1.log 2>&1 &
PID_P1=$!
sleep 2

# Wait for experiments to run
echo "Running experiments"
sleep 40

# Kill ALL processes
echo "Stopping shards..."
kill $PID_LOCALHOST $PID_LEARNER $PID_P2 $PID_P1 2>/dev/null
wait $PID_LOCALHOST $PID_LEARNER $PID_P2 $PID_P1 2>/dev/null

echo ""
echo "========================================="
echo "Checking test results..."
echo "========================================="

failed=0

# Check replay_batch counter in shard0-p1.log
echo ""
echo "Checking simple-shard0-p1.log:"
echo "-----------------"

if [ ! -f "simple-shard0-p1.log" ]; then
    echo "  ✗ simple-shard0-p1.log file not found"
    failed=1
else
    # Get the last occurrence of replay_batch
    last_replay_batch=$(grep "replay_batch:" "simple-shard0-p1.log" | tail -1)
    
    if [ -z "$last_replay_batch" ]; then
        echo "  ✗ No 'replay_batch' keyword found in simple-shard0-p1.log"
        failed=1
    else
        # Extract the replay_batch number (assuming format: "replay_batch:XXX")
        replay_count=$(echo "$last_replay_batch" | sed -n 's/.*replay_batch:\([0-9]*\).*/\1/p')
        
        if [ -z "$replay_count" ]; then
            echo "  ✗ Could not extract replay_batch value"
            echo "    Last line: $last_replay_batch"
            failed=1
        else
            # Check if replay_count is greater than 0
            if [ "$replay_count" -gt 0 ]; then
                echo "  ✓ replay_batch: $replay_count (> 0)"
            else
                echo "  ✗ replay_batch: $replay_count (should be > 0)"
                failed=1
            fi
        fi
    fi
fi

# Check all 4 logs for data integrity verification
echo ""
echo "Checking data integrity verification in all logs:"
echo "-----------------"
for log_suffix in localhost learner p2 p1; do
    log="simple-shard0-${log_suffix}.log"

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
    echo "Check simple-shard0-localhost.log and simple-shard0-p1.log for details"
    echo ""
    echo "Last 10 lines of simple-shard0-localhost.log:"
    tail -10 simple-shard0-localhost.log
    echo ""
    echo "Last 5 lines with 'replay_batch' from simple-shard0-p1.log:"
    grep "replay_batch" simple-shard0-p1.log | tail -5 2>/dev/null || echo "No replay_batch entries found"
    exit 1
fi