#!/bin/bash

# Script to test 2-shard experiments without replication
# Each shard should:
# 1. Show "agg_persist_throughput" keyword
# 2. Have NewOrder_remote_abort_ratio < 20%

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
source "${SCRIPT_DIR}/simple_transaction_rep_port_utils.sh"

echo "========================================="
echo "Testing 2-shard setup without replication"
echo "========================================="

# Clean up old log files
rm -f nfs_sync_*

# Clean up RocksDB data from previous runs
USERNAME=${USER:-$(whoami)}
rm -rf /tmp/${USERNAME}_mako_rocksdb_shard*

trd=${1:-6}
script_name="$(basename "$0")"
SHARD0_PID=""
SHARD1_PID=""
CLEANUP_DONE=0

# Use a randomized port base to avoid collisions on shared hosts.
TEMP_CONFIG=$(make_simple_txn_rep_config 2 $trd)
if [ -z "$TEMP_CONFIG" ]; then
    exit 1
fi
export MAKO_CONFIG="$TEMP_CONFIG"
echo "dbtest config: $MAKO_CONFIG"

cleanup_temp_config() {
    if [ "$CLEANUP_DONE" -eq 1 ]; then
        return
    fi
    CLEANUP_DONE=1

    # Stop any started shard wrappers.
    for pid in "${SHARD0_PID:-}" "${SHARD1_PID:-}"; do
        if [ -n "$pid" ]; then
            kill "$pid" 2>/dev/null || true
        fi
    done

    # Best-effort cleanup for dbtest processes tied to this run's unique temp config.
    # This prevents leaked workers when the script exits early (timeout/Ctrl-C).
    if [ -n "${TEMP_CONFIG:-}" ]; then
        pkill -TERM -f "$TEMP_CONFIG" 2>/dev/null || true
        sleep 1
        pkill -9 -f "$TEMP_CONFIG" 2>/dev/null || true
    fi

    for pid in "${SHARD0_PID:-}" "${SHARD1_PID:-}"; do
        if [ -n "$pid" ]; then
            wait "$pid" 2>/dev/null || true
        fi
    done

    rm -f "$TEMP_CONFIG"
    unset MAKO_CONFIG
}

handle_interrupt() {
    cleanup_temp_config
    exit 130
}

trap cleanup_temp_config EXIT
trap handle_interrupt INT TERM

# Determine transport type and create unique log prefix
transport="${MAKO_TRANSPORT:-rrr}"
log_prefix="${script_name}_${transport}"
log_file0="${log_prefix}_shard0-$trd.log"
log_file1="${log_prefix}_shard1-$trd.log"

ps aux | grep -i dbtest | awk "{print \$2}" | xargs kill -9 2>/dev/null
sleep 1
# Start shard 0 in background
echo "Starting shard 0..."
nohup bash bash/shard.sh 2 0 $trd localhost > "$log_file0" 2>&1 &
SHARD0_PID=$!
sleep 5

# Start shard 1 in background (delayed start ensures shard1 stays running while shard0 shuts down)
echo "Starting shard 1..."
nohup bash bash/shard.sh 2 1 $trd localhost > "$log_file1" 2>&1 &
SHARD1_PID=$!

# Wait for benchmarks to complete (poll for completion markers)
max_wait="${MAKO_MAX_WAIT_SECONDS:-120}"
wait_count=0
echo "Waiting for benchmark completion (timeout: ${max_wait}s)..."

while [ "$wait_count" -lt "$max_wait" ]; do
    shard0_done=0
    shard1_done=0

    if [ -f "$log_file0" ] && grep -q "agg_persist_throughput" "$log_file0" 2>/dev/null; then
        shard0_done=1
    fi

    if [ -f "$log_file1" ] && grep -q "agg_persist_throughput" "$log_file1" 2>/dev/null; then
        shard1_done=1
    fi

    if [ "$shard0_done" -eq 1 ] && [ "$shard1_done" -eq 1 ]; then
        echo "Both benchmarks completed after ${wait_count}s"
        sleep 2
        break
    fi

    sleep 1
    wait_count=$((wait_count + 1))
    if [ $((wait_count % 10)) -eq 0 ]; then
        echo "  ... waiting (${wait_count}s elapsed, shard0=$shard0_done, shard1=$shard1_done)"
    fi
done

if [ "$wait_count" -ge "$max_wait" ]; then
    echo "Warning: Benchmarks did not complete within ${max_wait}s timeout"
fi

# Stop any remaining processes
echo "Stopping shards..."
kill $SHARD0_PID $SHARD1_PID 2>/dev/null || true
sleep 2
kill -9 $SHARD0_PID $SHARD1_PID 2>/dev/null || true
wait $SHARD0_PID $SHARD1_PID 2>/dev/null

echo ""
echo "========================================="
echo "Checking test results..."
echo "========================================="

failed=0

# Check each shard's output
for i in 0 1; do
    log="${log_prefix}_shard${i}-$trd.log"
    echo ""
    echo "Checking $log:"
    echo "-----------------"
    
    if [ ! -f "$log" ]; then
        echo "  ✗ Log file not found"
        failed=1
        continue
    fi
    
    # Check for TPC-C sharding policy initialization (only for shard 0, as policy is shared)
    if [ "$i" -eq 0 ]; then
        if grep -q "TPC-C Sharding: Initialized policy" "$log"; then
            echo "  ✓ TPC-C sharding policy initialized"
            # Show the initialization line for reference
            grep "TPC-C Sharding: Initialized policy" "$log" | tail -n 1 | sed 's/^/    /'
        else
            echo "  ✗ TPC-C sharding policy not initialized"
            failed=1
        fi
    fi

    # Check for agg_persist_throughput keyword
    if grep -q "agg_persist_throughput" "$log"; then
        echo "  ✓ Found 'agg_persist_throughput' keyword"
        # Show the line for reference
        grep "agg_persist_throughput" "$log" | tail -n 1 | sed 's/^/    /'
    else
        echo "  ✗ 'agg_persist_throughput' keyword not found"
        failed=1
    fi
    
    # Check NewOrder_remote_abort_ratio
    if grep -q "NewOrder_remote_abort_ratio:" "$log"; then
        # Extract the abort ratio value
        abort_ratio=$(grep "NewOrder_remote_abort_ratio:" "$log" | tail -n 1 | awk '{print $2}')
        
        if [ -z "$abort_ratio" ]; then
            echo "  ✗ Could not extract NewOrder_remote_abort_ratio value"
            failed=1
        else
            # Remove % sign if present and convert to float
            abort_value=$(echo "$abort_ratio" | sed 's/%//')
            
            # Check if value is less than 20 using awk (more portable than bc)
            if awk "BEGIN {exit !($abort_value < 20)}"; then
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
    echo "Check ${log_prefix}_shard*-$trd for details"
    tail -n 10 ${log_prefix}_shard0-$trd.log ${log_prefix}_shard1-$trd.log
    exit 1
fi
