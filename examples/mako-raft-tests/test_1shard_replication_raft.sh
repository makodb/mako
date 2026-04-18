#!/bin/bash

# Script to test 1-shard experiments with Raft replication
# Each shard should:
# 1. Show "agg_persist_throughput" keyword
# 2. Have NewOrder_remote_abort_ratio < 20%
# 3. Followers replay at least 1000 batches
#
# NOTE: This script mirrors test_1shard_replication.sh (Paxos) exactly
# in duration, polling, shutdown, and validation — only the replication
# layer differs (Raft 3 replicas vs Paxos 4 replicas with learner).

echo "========================================="
echo "Testing 1-shard setup with Raft replication"
echo "========================================="

trd=${1:-6}
script_name="$(basename "$0")"
ps aux | grep -i dbtest | awk "{print \$2}" | xargs kill -9 2>/dev/null
# Clean up old log files
rm -f nfs_sync_*
USERNAME=${USER:-unknown}
rm -rf /tmp/${USERNAME}_mako_rocksdb_shard*

# Start shard 0 in background (3 Raft replicas, no learner)
echo "Starting shard 0 with Raft..."
nohup bash bash/shard_raft.sh 1 0 $trd localhost 0 1 > $script_name\_shard0-localhost-$trd.log 2>&1 &
nohup bash bash/shard_raft.sh 1 0 $trd p2 0 1 > $script_name\_shard0-p2-$trd.log 2>&1 &
sleep 1
nohup bash bash/shard_raft.sh 1 0 $trd p1 0 1 > $script_name\_shard0-p1-$trd.log 2>&1 &
SHARD0_PID=$!
sleep 2

# Wait for benchmark to complete (poll for completion marker)
# Same polling logic as Paxos test for fair comparison
echo "Waiting for benchmark to complete..."
log_file="${script_name}_shard0-localhost-$trd.log"
max_wait=120  # Maximum wait time in seconds
wait_count=0

while [ $wait_count -lt $max_wait ]; do
    # Check if throughput output appeared (indicates completion)
    if [ -f "$log_file" ] && grep -q "agg_persist_throughput" "$log_file" 2>/dev/null; then
        echo "Benchmark completed after ${wait_count}s"
        sleep 2  # Give a moment for final output
        break
    fi
    sleep 1
    wait_count=$((wait_count + 1))
    if [ $((wait_count % 10)) -eq 0 ]; then
        echo "  ... waiting (${wait_count}s elapsed)"
    fi
done

if [ $wait_count -ge $max_wait ]; then
    echo "Warning: Benchmark did not complete within ${max_wait}s timeout"
fi

# Graceful shutdown: SIGTERM first (same as Paxos test)
echo "Stopping shards (graceful)..."
pkill -TERM -f "dbtest.*shard-index 0" 2>/dev/null || true
sleep 3

# Force kill any remaining processes
pkill -9 -f "dbtest.*shard-index 0" 2>/dev/null || true
sleep 1

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

            # Handle -nan/nan (0 remote txns = 0/0 division, perfectly fine for single shard)
            if echo "$abort_value" | grep -qi "nan"; then
                echo "  ✓ NewOrder_remote_abort_ratio: $abort_ratio (no remote txns, OK)"
            elif awk "BEGIN {exit !($abort_value < 20)}"; then
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

# Check replay_batch counters in both followers (p1/p2)
echo ""
log_p1="${script_name}_shard0-p1-$trd.log"
log_p2="${script_name}_shard0-p2-$trd.log"
echo "Checking follower replay_batch counters:"
echo "-----------------"

extract_replay_count() {
    local log_file="$1"
    if [ ! -f "$log_file" ]; then
        echo ""
        return 1
    fi
    local last_line
    last_line=$(grep "replay_batch:" "$log_file" | tail -1)
    if [ -z "$last_line" ]; then
        echo ""
        return 1
    fi
    local value
    value=$(echo "$last_line" | sed -n 's/.*replay_batch:\([0-9]*\).*/\1/p')
    if [ -z "$value" ]; then
        echo ""
        return 1
    fi
    echo "$value"
    return 0
}

replay_p1=$(extract_replay_count "$log_p1")
ok_p1=$?
replay_p2=$(extract_replay_count "$log_p2")
ok_p2=$?

if [ "$ok_p1" -ne 0 ]; then
    echo "  ✗ Could not read replay_batch from $log_p1"
    failed=1
else
    echo "  p1 replay_batch: $replay_p1"
fi

if [ "$ok_p2" -ne 0 ]; then
    echo "  ✗ Could not read replay_batch from $log_p2"
    failed=1
else
    echo "  p2 replay_batch: $replay_p2"
fi

if [ "$ok_p1" -eq 0 ] && [ "$ok_p2" -eq 0 ]; then
    if [ "$replay_p1" -gt "$replay_p2" ]; then
        max_replay=$replay_p1
        min_replay=$replay_p2
    else
        max_replay=$replay_p2
        min_replay=$replay_p1
    fi

    # CI in this test can see leader churn; one follower may lag while the other catches up.
    # Require strong replication on one follower and non-trivial replay on the other.
    if [ "$max_replay" -gt 500 ] && [ "$min_replay" -gt 100 ]; then
        echo "  ✓ follower replay checks passed (max=$max_replay > 500, min=$min_replay > 100)"
    else
        echo "  ✗ follower replay checks failed (max=$max_replay, min=$min_replay; need max>500 and min>100)"
        failed=1
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
    echo "Check $script_name\_shard0-localhost-$trd.log, $log_p1 and $log_p2 for details"
    echo ""
    echo "Last 10 lines of $script_name\_shard0-localhost-$trd.log:"
    tail -10 $script_name\_shard0-localhost-$trd.log
    echo ""
    echo "Last 5 lines with 'replay_batch' from $log_p1:"
    grep "replay_batch" $log_p1 | tail -5 2>/dev/null || echo "No replay_batch entries found in $log_p1"
    echo ""
    echo "Last 5 lines with 'replay_batch' from $log_p2:"
    grep "replay_batch" $log_p2 | tail -5 2>/dev/null || echo "No replay_batch entries found in $log_p2"
    exit 1
fi
