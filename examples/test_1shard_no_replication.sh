#!/bin/bash

# Script to test 1-shard experiments without replication.
# Success criteria:
# 1. Show "agg_persist_throughput" keyword
# 2. NewOrder_remote_abort_ratio is < 20%, or N/A when no remote txns occur

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
source "${SCRIPT_DIR}/simple_transaction_rep_port_utils.sh"

echo "========================================="
echo "Testing 1-shard setup without replication"
echo "========================================="

trd=${1:-${MAKO_CI_TRD:-6}}
script_name="$(basename "$0")"
binary_path="./${BUILD_DIR:-build}/dbtest"
SHARD0_PID=""
CLEANUP_DONE=0

if [ ! -x "$binary_path" ]; then
    echo "Error: dbtest binary not found or not executable at '$binary_path'"
    echo "Build it first (for Docker: ./docker_build.sh build), then retry."
    exit 1
fi

# Clean up old log files
rm -f nfs_sync_*

# Clean up RocksDB data from previous runs
USERNAME=${USER:-$(whoami)}
rm -rf /tmp/${USERNAME}_mako_rocksdb_shard*

# Use a randomized port base to avoid collisions on shared hosts.
TEMP_CONFIG=$(make_simple_txn_rep_config 1 $trd)
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

    # Stop any started shard wrapper/process.
    if [ -n "${SHARD0_PID:-}" ]; then
        kill "$SHARD0_PID" 2>/dev/null || true
        sleep 1
        kill -9 "$SHARD0_PID" 2>/dev/null || true
        wait "$SHARD0_PID" 2>/dev/null || true
    fi

    # Best-effort cleanup for dbtest workers tied to this run's unique temp config.
    if [ -n "${TEMP_CONFIG:-}" ]; then
        pkill -TERM -f "$TEMP_CONFIG" 2>/dev/null || true
        sleep 1
        pkill -9 -f "$TEMP_CONFIG" 2>/dev/null || true
    fi

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
transport="rrr"
log_prefix="${script_name}_${transport}"
log_file="${log_prefix}_shard0-$trd.log"

# Ensure this run uses a fresh writable log file to avoid stale false positives.
if [ -f "$log_file" ]; then
    rm -f "$log_file" 2>/dev/null || true
fi
if ! : > "$log_file"; then
    echo "Error: Cannot write log file '$log_file'."
    echo "Fix file permissions or remove stale files, then retry."
    exit 1
fi

# Kill only dbtest worker processes by executable name.
# Avoid grep/xargs patterns that can match wrapper shells containing "dbtest" in argv.
pkill -9 -x dbtest 2>/dev/null || true
sleep 1

# Start shard 0 in background
echo "Starting shard 0..."
nohup bash bash/shard.sh 1 0 $trd localhost > "$log_file" 2>&1 &
SHARD0_PID=$!
sleep 2

# Wait for benchmark completion (poll for completion marker)
max_wait="${MAKO_MAX_WAIT_SECONDS:-120}"
if ! [[ "$max_wait" =~ ^[0-9]+$ ]] || [ "$max_wait" -le 0 ]; then
    echo "Warning: MAKO_MAX_WAIT_SECONDS='${max_wait}' is invalid; using default 120s"
    max_wait=120
fi
wait_count=0
benchmark_completed=0
timed_out=0
process_exited_early=0
echo "Waiting for benchmark completion (timeout: ${max_wait}s)..."
while [ "$wait_count" -lt "$max_wait" ]; do
    if [ -f "$log_file" ] && grep -q "agg_persist_throughput" "$log_file" 2>/dev/null; then
        echo "Benchmark completed after ${wait_count}s"
        benchmark_completed=1
        sleep 2
        break
    fi

    if ! kill -0 "$SHARD0_PID" 2>/dev/null; then
        sleep 1
        if [ -f "$log_file" ] && grep -q "agg_persist_throughput" "$log_file" 2>/dev/null; then
            echo "Benchmark completed after ${wait_count}s (process exited after writing results)"
            benchmark_completed=1
            sleep 1
            break
        fi
        echo "Shard 0 process exited unexpectedly after ${wait_count}s"
        process_exited_early=1
        break
    fi

    sleep 1
    wait_count=$((wait_count + 1))
    if [ $((wait_count % 10)) -eq 0 ]; then
        echo "  ... waiting (${wait_count}s elapsed)"
    fi
done

if [ "$wait_count" -ge "$max_wait" ] && [ "$benchmark_completed" -eq 0 ]; then
    echo "Warning: Benchmark did not complete within ${max_wait}s timeout"
    timed_out=1
fi

# Stop process (graceful first, force if still alive)
echo "Stopping shard..."
kill "$SHARD0_PID" 2>/dev/null || true
sleep 2
if kill -0 "$SHARD0_PID" 2>/dev/null; then
    kill -9 "$SHARD0_PID" 2>/dev/null || true
fi
wait "$SHARD0_PID" 2>/dev/null

echo ""
echo "========================================="
echo "Checking test results..."
echo "========================================="

failed=0

if [ "$process_exited_early" -eq 1 ]; then
    echo "  ✗ Shard process exited before benchmark completion"
    failed=1
fi

if [ "$timed_out" -eq 1 ]; then
    echo "  ✗ Benchmark timed out before throughput was observed"
    failed=1
fi

echo ""
echo "Checking $log_file:"
echo "-----------------"

if [ ! -f "$log_file" ]; then
    echo "  ✗ Log file not found"
    exit 1
fi

# Check for TPC-C sharding policy initialization
if grep -q "TPC-C Sharding: Initialized policy" "$log_file"; then
    echo "  ✓ TPC-C sharding policy initialized"
    grep "TPC-C Sharding: Initialized policy" "$log_file" | tail -1 | sed 's/^/    /'
else
    echo "  ✗ TPC-C sharding policy not initialized"
    failed=1
fi

# Check for throughput output
if grep -q "agg_persist_throughput" "$log_file"; then
    echo "  ✓ Found 'agg_persist_throughput' keyword"
    grep "agg_persist_throughput" "$log_file" | tail -1 | sed 's/^/    /'
else
    echo "  ✗ 'agg_persist_throughput' keyword not found"
    failed=1
fi

# Check NewOrder_remote_abort_ratio
if grep -q "NewOrder_remote_abort_ratio:" "$log_file"; then
    abort_ratio=$(grep "NewOrder_remote_abort_ratio:" "$log_file" | tail -1 | awk '{print $2}')
    if [ -z "$abort_ratio" ]; then
        echo "  ✗ Could not extract NewOrder_remote_abort_ratio value"
        failed=1
    else
        abort_value=$(echo "$abort_ratio" | sed 's/%//')
        abort_value_lower=$(echo "$abort_value" | tr '[:upper:]' '[:lower:]')

        if [ "$abort_value_lower" = "nan" ] || [ "$abort_value_lower" = "-nan" ]; then
            echo "  ✓ NewOrder_remote_abort_ratio: $abort_ratio (N/A: no remote transactions in single-shard run)"
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
    echo "Check $log_file for details"
    tail -n 20 "$log_file"
    exit 1
fi
