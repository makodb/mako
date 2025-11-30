#!/bin/bash

# Script to test 2-shard experiments with replication using simpleTransactionRep

echo "========================================="
echo "Testing 2-shard setup with replication using simpleTransactionRep"
echo "========================================="

#skill dbtest
# Clean up old log files
rm -f nfs_sync_*
rm -f simple-shard0*.log simple-shard1*.log
USERNAME=${USER:-unknown}
rm -rf /tmp/${USERNAME}_mako_rocksdb_shard*

ps aux | grep -i dbtest | awk "{print \$2}" | xargs kill -9 2>/dev/null
ps aux | grep -i simpleTransactionRep | awk "{print \$2}" | xargs kill -9 2>/dev/null
sleep 1

# Start shard 0 in background - capture ALL PIDs
echo "Starting shard 0..."
trd=6
nohup ./build/simpleTransactionRep 2 0 $trd localhost 1 > simple-shard0-localhost.log 2>&1 &
PID_S0_LOCALHOST=$!
nohup ./build/simpleTransactionRep 2 0 $trd learner 1 > simple-shard0-learner.log 2>&1 &
PID_S0_LEARNER=$!
nohup ./build/simpleTransactionRep 2 0 $trd p2 1 > simple-shard0-p2.log 2>&1 &
PID_S0_P2=$!
sleep 1
nohup ./build/simpleTransactionRep 2 0 $trd p1 1  > simple-shard0-p1.log 2>&1 &
PID_S0_P1=$!

sleep 2

# Start shard 1 in background - capture ALL PIDs
echo "Starting shard 1..."
nohup ./build/simpleTransactionRep 2 1 $trd localhost 1 > simple-shard1-localhost.log 2>&1 &
PID_S1_LOCALHOST=$!
nohup ./build/simpleTransactionRep 2 1 $trd learner 1 > simple-shard1-learner.log 2>&1 &
PID_S1_LEARNER=$!
nohup ./build/simpleTransactionRep 2 1 $trd p2 1 > simple-shard1-p2.log 2>&1 &
PID_S1_P2=$!
sleep 1
nohup ./build/simpleTransactionRep 2 1 $trd p1 1  > simple-shard1-p1.log 2>&1 &
PID_S1_P1=$!

# Wait for experiments to run
echo "Running experiments for 30 seconds..."
sleep 60

# Kill ALL processes from both shards
echo "Stopping shards..."
kill $PID_S0_LOCALHOST $PID_S0_LEARNER $PID_S0_P2 $PID_S0_P1 \
     $PID_S1_LOCALHOST $PID_S1_LEARNER $PID_S1_P2 $PID_S1_P1 2>/dev/null
wait $PID_S0_LOCALHOST $PID_S0_LEARNER $PID_S0_P2 $PID_S0_P1 \
     $PID_S1_LOCALHOST $PID_S1_LEARNER $PID_S1_P2 $PID_S1_P1 2>/dev/null

echo ""
echo "========================================="
echo "Checking test results..."
echo "========================================="

failed=0

# Check each shard's output
for i in 0 1; do
    log="simple-shard${i}-p1.log"
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
            # Check if replay_count is greater than 0
            if [ "$replay_count" -gt 0 ]; then
                echo "  ✓ replay_batch: $replay_count (> 0)"
            else
                echo "  ✗ replay_batch: $replay_count (should be > 0)"
                failed=1
            fi
        fi
    fi
    
done

# Check all 8 logs for data integrity verification
echo ""
echo "Checking data integrity verification in all logs:"
echo "-----------------"
for shard in 0 1; do
    for log_suffix in localhost learner p2 p1; do
        log="simple-shard${shard}-${log_suffix}.log"

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
    echo "Check simple-shard0-localhost.log and simple-shard1-localhost.log for details"
    tail -10 simple-shard0-localhost.log
    tail -10 simple-shard1-localhost.log
    exit 1
fi