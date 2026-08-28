#!/bin/bash

# Script to test 2-shard single process mode WITHOUT replication
# This tests running shards 0 and 1 in a single process using the -L flag
#
# Success criteria:
# 1. Show "Multi-shard mode: running 2 shards in this process"
# 2. Show "Created shared SiloRuntime" for multi-shard mode
# 3. Show "Initialized ShardContext for shard" for each shard
# 4. Show "agg_persist_throughput" keyword

# Source common utilities (includes GDB_PREFIX for debugging)
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
source "${SCRIPT_DIR}/../bash/util.sh"
source "${SCRIPT_DIR}/simple_transaction_rep_port_utils.sh"

echo "========================================="
echo "Testing 2-shard single process mode (no replication)"
echo "========================================="

if [ "$GDB_ENABLED" == "1" ]; then
    echo "[GDB] Debug mode enabled"
fi

# Clean up old log files
rm -f nfs_sync_*
USERNAME=${USER:-unknown}
rm -rf /tmp/${USERNAME}_mako_rocksdb_shard*

trd=${1:-${MAKO_CI_TRD:-6}}
script_name="$(basename "$0")"
binary_path="./${BUILD_DIR:-build}/dbtest"
PROCESS_PID=""
CLEANUP_DONE=0

if [ ! -x "$binary_path" ]; then
    echo "Error: dbtest binary not found or not executable at '$binary_path'"
    echo "Build it first (for Docker: ./docker_build.sh build), then retry."
    exit 1
fi

cleanup_process() {
    if [ "$CLEANUP_DONE" -eq 1 ]; then
        return
    fi
    CLEANUP_DONE=1

    if [ -n "${PROCESS_PID:-}" ]; then
        kill "$PROCESS_PID" 2>/dev/null || true
        sleep 1
        kill -9 "$PROCESS_PID" 2>/dev/null || true
        wait "$PROCESS_PID" 2>/dev/null || true
    fi

    if [ -n "${TEMP_CONFIG:-}" ]; then
        rm -f "$TEMP_CONFIG"
    fi
    unset MAKO_CONFIG
}

handle_interrupt() {
    cleanup_process
    exit 130
}

trap cleanup_process EXIT
trap handle_interrupt INT TERM

# Determine transport type and create unique log prefix
transport="rrr"
log_prefix="${script_name}_${transport}"
log_file="${log_prefix}_2shard_single-$trd.log"

# Kill only dbtest worker processes by executable name.
# Avoid grep/xargs patterns that can match wrapper shells containing "dbtest" in argv.
pkill -9 -x dbtest 2>/dev/null || true
sleep 1

path=$(pwd)/src/mako

# Randomize the shard config so the hardcoded 31000/31100 ports don't collide
# with leftover TIME_WAIT sockets from earlier CI tests.
TEMP_CONFIG=$(make_simple_txn_rep_config 2 "$trd")
if [ -z "$TEMP_CONFIG" ]; then
    echo "Error: Failed to materialize randomized shard config" >&2
    exit 1
fi
export MAKO_CONFIG="$TEMP_CONFIG"
echo "shard config: $MAKO_CONFIG"

# Build the command for 2-shard single process mode (no replication)
# Key: -L 0,1 specifies running shards 0 and 1 in the same process
CMD="./${BUILD_DIR:-build}/dbtest --num-threads $trd --shard-config $TEMP_CONFIG -P localhost -L 0,1"
THROTTLE_ARGS="$(mako_dbtest_throttle_args)" || exit 1
if [ -n "$THROTTLE_ARGS" ]; then
    CMD="$CMD$THROTTLE_ARGS"
fi

echo ""
echo "Configuration:"
echo "-----------------"
echo "  Number of threads: $trd"
echo "  Local shards:      0,1 (single process mode)"
echo "  Replication:       disabled"
echo "  Config file:       $TEMP_CONFIG (randomized from $path/config/local-shards2-warehouses$trd.yml)"
if [ -n "${MAKO_CPU_LIMIT:-}" ]; then
    echo "  CPU throttle:      ${MAKO_CPU_LIMIT}% (cycle=${MAKO_THROTTLE_CYCLE_MS:-default}ms)"
else
    echo "  CPU throttle:      disabled"
fi
echo "  Log file:          $log_file"
echo ""
echo "Command: $CMD"
echo ""

# Start 2-shard single process in background
echo "Starting 2-shard single process..."
nohup $GDB_PREFIX $CMD > $log_file 2>&1 &
PROCESS_PID=$!
sleep 2

# Wait for benchmark to complete (check for throughput output)
echo "Waiting for benchmark to complete..."
max_wait="${MAKO_MAX_WAIT_SECONDS:-90}"
if ! [[ "$max_wait" =~ ^[0-9]+$ ]] || [ "$max_wait" -le 0 ]; then
    echo "Warning: MAKO_MAX_WAIT_SECONDS='${max_wait}' is invalid; using default 90s"
    max_wait=90
fi
wait_count=0
benchmark_completed=0
timed_out=0
process_exited_early=0

while [ "$wait_count" -lt "$max_wait" ]; do
    if [ -f "$log_file" ] && grep -q "agg_persist_throughput" "$log_file" 2>/dev/null; then
        echo "Benchmark completed after ${wait_count}s"
        benchmark_completed=1
        sleep 2
        break
    fi

    if ! kill -0 "$PROCESS_PID" 2>/dev/null; then
        # Process may exit immediately after writing final metrics.
        sleep 1
        if [ -f "$log_file" ] && grep -q "agg_persist_throughput" "$log_file" 2>/dev/null; then
            echo "Benchmark completed after ${wait_count}s (process exited after writing results)"
            benchmark_completed=1
            sleep 1
            break
        fi
        echo "Process exited unexpectedly after ${wait_count}s"
        process_exited_early=1
        break
    fi

    sleep 1
    wait_count=$((wait_count + 1))
    if [ $((wait_count % 10)) -eq 0 ]; then
        echo "  ... waiting (${wait_count}s elapsed)"
    fi
done

if [ "$benchmark_completed" -eq 0 ] && [ "$wait_count" -ge "$max_wait" ]; then
    echo "Warning: Benchmark did not complete within ${max_wait}s timeout"
    timed_out=1
fi

# Kill the process
echo "Stopping process..."
kill "$PROCESS_PID" 2>/dev/null
wait "$PROCESS_PID" 2>/dev/null

echo ""
echo "========================================="
echo "Checking test results..."
echo "========================================="

failed=0

if [ "$process_exited_early" -eq 1 ]; then
    echo "  X Process exited before benchmark completion"
    failed=1
fi

if [ "$timed_out" -eq 1 ]; then
    echo "  X Benchmark timed out before throughput was observed"
    failed=1
fi

echo ""
echo "Checking $log_file:"
echo "-----------------"

if [ ! -f "$log_file" ]; then
    echo "  X Log file not found"
    exit 1
fi

# Check for TPC-C sharding policy initialization
if grep -q "TPC-C Sharding: Initialized policy" "$log_file"; then
    echo "  OK TPC-C sharding policy initialized"
    # Show the initialization line for reference
    grep "TPC-C Sharding: Initialized policy" "$log_file" | tail -n 1 | sed 's/^/    /'
else
    echo "  X TPC-C sharding policy not initialized"
    failed=1
fi

# Check 1: Multi-shard mode initialization
if grep -q "Multi-shard mode: running 2 shards in this process" "$log_file"; then
    echo "  OK Multi-shard mode initialization detected (2 shards)"
else
    echo "  X Multi-shard mode initialization not found"
    failed=1
fi

# Check 2: Shared SiloRuntime creation
if grep -q "Created shared SiloRuntime" "$log_file"; then
    echo "  OK Shared SiloRuntime created for multi-shard mode"
    grep "Created shared SiloRuntime" "$log_file" | head -1 | sed 's/^/    /'
else
    echo "  X Shared SiloRuntime not found"
    failed=1
fi

# Check 3: ShardContext initialization for shard 0
if grep -q "Initialized ShardContext for shard 0" "$log_file"; then
    echo "  OK ShardContext initialized for shard 0"
else
    echo "  X ShardContext for shard 0 not initialized"
    failed=1
fi

# Check 4: ShardContext initialization for shard 1
if grep -q "Initialized ShardContext for shard 1" "$log_file"; then
    echo "  OK ShardContext initialized for shard 1"
else
    echo "  X ShardContext for shard 1 not initialized"
    failed=1
fi

# Check 5: Workers running in parallel for shard 0
if grep -q "Running workers for shard 0 in thread" "$log_file"; then
    echo "  OK Workers running in parallel thread for shard 0"
else
    echo "  X Workers not running in thread for shard 0"
    failed=1
fi

# Check 6: Workers running in parallel for shard 1
if grep -q "Running workers for shard 1 in thread" "$log_file"; then
    echo "  OK Workers running in parallel thread for shard 1"
else
    echo "  X Workers not running in thread for shard 1"
    failed=1
fi

# Check 7: Throughput output (required for success)
if grep -q "agg_persist_throughput" "$log_file"; then
    echo "  OK Found 'agg_persist_throughput' keyword"
    grep "agg_persist_throughput" "$log_file" | tail -1 | sed 's/^/    /'
else
    echo "  X 'agg_persist_throughput' keyword not found"
    failed=1
fi

echo ""
echo "========================================="
if [ $failed -eq 0 ]; then
    echo "All checks passed!"
    echo "2-shard single process mode (no replication) working correctly."
    echo "========================================="
    exit 0
else
    echo "Some checks failed!"
    echo "========================================="
    echo ""
    echo "Debug information:"
    echo "Check $log_file for details"
    echo ""
    echo "Last 20 lines of $log_file:"
    tail -20 "$log_file"
    exit 1
fi
