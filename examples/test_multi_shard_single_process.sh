#!/bin/bash

# Script to test multi-shard single process mode
# This tests running multiple shards (0 and 1) in a single process using the -L flag
#
# Success requires completed positive throughput from both owners, not
# initialization log wording. The native migration smoke separately asserts
# owner-isolated physical data, admission and handoff behavior.

# Source common utilities (includes GDB_PREFIX for debugging)
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
source "${SCRIPT_DIR}/../bash/util.sh"
source "${SCRIPT_DIR}/simple_transaction_rep_port_utils.sh"

echo "========================================="
echo "Testing multi-shard single process mode"
echo "========================================="

if [ "$GDB_ENABLED" == "1" ]; then
    echo "[GDB] Debug mode enabled"
fi

# Clean up old log files
rm -f nfs_sync_*

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
transport="srpc"
log_prefix="${script_name}_${transport}"
log_file="${log_prefix}_multi_shard-$trd.log"

# Kill only dbtest worker processes by executable name.
# Avoid grep/xargs patterns that can match wrapper shells containing "dbtest" in argv.
pkill -9 -x dbtest 2>/dev/null || true
sleep 1

path=$(pwd)/src/mako

# Randomize the shard config so the hardcoded 31000/31100 ports don't collide
# with leftover TIME_WAIT sockets from earlier CI tests (simpleTransaction
# picks bases up to 28599 + offset 3100 = 31699, overlapping with our 31000).
TEMP_CONFIG=$(make_simple_txn_rep_config 2 "$trd")
if [ -z "$TEMP_CONFIG" ]; then
    echo "Error: Failed to materialize randomized shard config" >&2
    exit 1
fi
export MAKO_CONFIG="$TEMP_CONFIG"
echo "shard config: $MAKO_CONFIG"

# Build the command for multi-shard single process mode
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
echo "  Local shards:      0,1 (multi-shard mode)"
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

# Start multi-shard process in background
echo "Starting multi-shard single process..."
nohup $GDB_PREFIX $CMD > "$log_file" 2>&1 &
PROCESS_PID=$!
sleep 2

# Wait for both owners to finish before stopping their shared process.
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
    if [ -f "$log_file" ] && [ "$(grep -c "agg_persist_throughput:" "$log_file" 2>/dev/null)" -ge 2 ]; then
        echo "Benchmark completed after ${wait_count}s"
        benchmark_completed=1
        sleep 2
        break
    fi

    if ! kill -0 "$PROCESS_PID" 2>/dev/null; then
        # Process may exit immediately after writing final metrics.
        sleep 1
        if [ -f "$log_file" ] && [ "$(grep -c "agg_persist_throughput:" "$log_file" 2>/dev/null)" -ge 2 ]; then
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

if [ "$wait_count" -ge "$max_wait" ] && [ "$benchmark_completed" -eq 0 ]; then
    echo "Warning: Benchmark did not complete within ${max_wait}s timeout"
    timed_out=1
fi

# Stop process (graceful first, force if still alive)
echo "Stopping process..."
kill "$PROCESS_PID" 2>/dev/null || true
sleep 2
if kill -0 "$PROCESS_PID" 2>/dev/null; then
    kill -9 "$PROCESS_PID" 2>/dev/null || true
fi
wait "$PROCESS_PID" 2>/dev/null

echo ""
echo "========================================="
echo "Checking test results..."
echo "========================================="

failed=0

if [ "$process_exited_early" -eq 1 ]; then
    echo "  ✗ Process exited before benchmark completion"
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

# Both owners must complete useful work.
if ! python3 - "$log_file" <<'PY'
import math
import re
import sys
from pathlib import Path

rates = [float(value) for value in re.findall(
    r"agg_persist_throughput:\s*([0-9.eE+-]+)\s*ops/sec",
    Path(sys.argv[1]).read_text(errors="replace"))]
if len(rates) != 2 or not all(math.isfinite(rate) and rate > 0 for rate in rates):
    raise SystemExit(f"Expected two positive completed owner throughputs, got {rates}")
print(f"Both owners completed transactions: {rates} ops/sec")
PY
then
    failed=1
fi

echo ""
echo "========================================="
if [ $failed -eq 0 ]; then
    echo "All checks passed!"
    echo "Multi-shard single process mode is working correctly."
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
