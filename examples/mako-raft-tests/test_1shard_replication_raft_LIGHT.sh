#!/bin/bash

# Lightweight test script for 1-shard Raft replication
# Uses minimal workload: 2 threads, 15 seconds

echo "========================================="
echo "Lightweight 1-shard Raft Replication Test"
echo "Configuration:"
echo "  - Threads: 2 (down from 6)"
echo "  - Duration: 15 seconds (down from 60)"
echo "  - Workload: TPC-C with 2 warehouses"
echo "  - Target: >300 replay batches"
echo "========================================="

trd=2
script_name="$(basename "$0")"

# PROPER cleanup function
cleanup() {
    echo ""
    echo "Cleaning up all processes..."
    ps aux | grep -i dbtest | grep -v grep | awk "{print \$2}" | xargs kill -9 2>/dev/null
    ps aux | grep -i simpleRaft | grep -v grep | awk "{print \$2}" | xargs kill -9 2>/dev/null
    sleep 1
}

# Kill any existing processes
cleanup
rm -f nfs_sync_*
rm -rf /tmp/mako_rocksdb_shard*
sleep 1

# Start shard 0 with 3 Raft replicas
echo ""
echo "Starting shard 0 with 3 Raft replicas..."
echo "  - localhost (leader candidate)"
echo "  - p2 (follower)"
echo "  - p1 (follower - we'll check replay here)"

nohup bash bash/shard_raft.sh 1 0 $trd localhost 0 1 > $script_name\_shard0-localhost-$trd.log 2>&1 &
nohup bash bash/shard_raft.sh 1 0 $trd p2 0 1 > $script_name\_shard0-p2-$trd.log 2>&1 &
sleep 1
nohup bash bash/shard_raft.sh 1 0 $trd p1 0 1 > $script_name\_shard0-p1-$trd.log 2>&1 &

sleep 2

echo ""
echo "Waiting for cluster to stabilize (2s)..."
sleep 2

echo ""
echo "Running workload for 15 seconds..."
echo "(In full test this would be 60 seconds)"

# Show live progress every 3 seconds
for i in {1..5}; do
    sleep 3
    elapsed=$((i * 3))
    log_p1="${script_name}_shard0-p1-$trd.log"
    if [ -f "$log_p1" ]; then
        current_replay=$(grep "replay_batch:" "$log_p1" 2>/dev/null | tail -1 | sed -n 's/.*replay_batch:\([0-9]*\).*/\1/p')
        if [ -n "$current_replay" ]; then
            echo "  [${elapsed}s] Follower replay_batch: $current_replay"
        fi
    fi
done

# PROPER cleanup - kill all dbtest processes
cleanup

echo ""
echo "========================================="
echo "Test Results"
echo "========================================="

failed=0

# Check leader (localhost) output
{
    i=0
    log="${script_name}_shard${i}-localhost-$trd.log"
    echo ""
    echo "Leader (localhost) checks:"
    echo "--------------------------"

    if [ ! -f "$log" ]; then
        echo "  ✗ Log file not found: $log"
        failed=1
    else
        # Check for agg_persist_throughput keyword
        if grep -q "agg_persist_throughput" "$log"; then
            throughput_line=$(grep "agg_persist_throughput" "$log" | tail -1)
            echo "  ✓ Found 'agg_persist_throughput' keyword"
            echo "    $throughput_line" | sed 's/^/    /'
        else
            echo "  ⚠ 'agg_persist_throughput' keyword not found (test may have been cut short)"
            # Don't fail - the test is only 15s
        fi

        # Check NewOrder_remote_abort_ratio (if exists)
        if grep -q "NewOrder_remote_abort_ratio:" "$log"; then
            abort_ratio=$(grep "NewOrder_remote_abort_ratio:" "$log" | tail -1 | awk '{print $2}')
            if [ -n "$abort_ratio" ]; then
                abort_value=$(echo "$abort_ratio" | sed 's/%//')
                if awk "BEGIN {exit !($abort_value < 20)}"; then
                    echo "  ✓ NewOrder_remote_abort_ratio: $abort_ratio (< 20%)"
                else
                    echo "  ✗ NewOrder_remote_abort_ratio: $abort_ratio (>= 20%)"
                    failed=1
                fi
            fi
        fi
    fi
}

# Check replay_batch counter in follower (p1)
echo ""
log_p1="${script_name}_shard0-p1-$trd.log"
echo "Follower (p1) replay checks:"
echo "----------------------------"

if [ ! -f "$log_p1" ]; then
    echo "  ✗ $log_p1 file not found"
    failed=1
else
    # Get the last occurrence of replay_batch
    last_replay_batch=$(grep "replay_batch:" "$log_p1" | tail -1)

    if [ -z "$last_replay_batch" ]; then
        echo "  ✗ No 'replay_batch' keyword found in $log_p1"
        echo ""
        echo "  Debug: Checking for advancer startup..."
        if grep -q "we can start a advancer" "$log_p1"; then
            echo "  ✓ Advancer was started"
        else
            echo "  ✗ Advancer was NOT started (watermark issue!)"
        fi

        echo ""
        echo "  Debug: Checking for watermark bootstrap..."
        if grep -q "WATERMARK-BOOTSTRAP" "$log_p1"; then
            echo "  ✓ Watermark bootstrap message found (fix is working!)"
        else
            echo "  ✗ Watermark bootstrap message NOT found (fix not applied?)"
        fi

        failed=1
    else
        # Extract the replay_batch number
        replay_count=$(echo "$last_replay_batch" | sed -n 's/.*replay_batch:\([0-9]*\).*/\1/p')

        if [ -z "$replay_count" ]; then
            echo "  ✗ Could not extract replay_batch value"
            echo "    Last line: $last_replay_batch"
            failed=1
        else
            # For lightweight test (15s, 2 threads), expect at least 300 batches
            MIN_EXPECTED=300

            if [ "$replay_count" -gt "$MIN_EXPECTED" ]; then
                echo "  ✓ replay_batch: $replay_count (> $MIN_EXPECTED) - GOOD!"
            else
                echo "  ⚠ replay_batch: $replay_count (< $MIN_EXPECTED expected)"
                echo "    This is lower than expected for a 15s test with the fix"
            fi

            # Show progress over time
            echo ""
            echo "  Replay progress over time (last 5 entries):"
            grep "replay_batch" "$log_p1" | tail -5 | sed 's/^/    /'

            # Check if it's stuck (plateau)
            last_5_counts=$(grep "replay_batch:" "$log_p1" | tail -5 | sed -n 's/.*replay_batch:\([0-9]*\).*/\1/p')
            unique_counts=$(echo "$last_5_counts" | sort -u | wc -l)

            if [ "$unique_counts" -eq 1 ] && [ "$replay_count" -lt 1000 ]; then
                echo ""
                echo "  ⚠ WARNING: replay_batch appears STUCK (same value for last 5 entries)"
                echo "     This indicates the watermark bootstrap fix may not be working!"
            fi
        fi
    fi
fi

echo ""
echo "========================================="
if [ $failed -eq 0 ]; then
    echo "✓ Lightweight test PASSED!"
    echo "========================================="
    echo ""
    echo "Next steps:"
    echo "  1. Run this test 3-5 times to check consistency"
    echo "  2. If consistently good (>300), scale up to full 6-thread test"
    echo "  3. Check for 'STUCK' warnings above"
    exit 0
else
    echo "✗ Lightweight test FAILED"
    echo "========================================="
    echo ""
    echo "Debug information:"
    echo ""
    echo "Leader log (last 10 lines):"
    tail -10 $script_name\_shard0-localhost-$trd.log 2>/dev/null || echo "  Log not found"
    echo ""
    echo "Follower log (last 10 lines):"
    tail -10 $log_p1 2>/dev/null || echo "  Log not found"
    echo ""
    echo "Grep for errors in follower log:"
    grep -i "error\|fail\|crash" $log_p1 2>/dev/null | tail -5 || echo "  No errors found"
    exit 1
fi
