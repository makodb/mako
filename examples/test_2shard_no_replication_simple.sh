#!/bin/bash

# Script to test 2-shard experiments without replication using simpleTransactionRep

echo "========================================="
echo "Testing 2-shard setup without replication using simpleTransactionRep"
echo "========================================="

trd=${1:-6}
binary_path="./${BUILD_DIR:-build}/simpleTransactionRep"
log_shard0="simple-shard0-localhost.log"
log_shard1="simple-shard1-localhost.log"
completion_marker="All tests completed successfully!"
PROC_MATCH="[/]simpleTransactionRep( |$)"
SHARD0_PID=""
SHARD1_PID=""
CLEANUP_DONE=0

if [ ! -x "$binary_path" ]; then
    echo "Error: simpleTransactionRep binary not found or not executable at '$binary_path'"
    echo "Build it first (for Docker: ./docker_build.sh build), then retry."
    exit 1
fi

# Clean up old files and processes
rm -f nfs_sync_*
rm -f "$log_shard0" "$log_shard1"
USERNAME=${USER:-unknown}
rm -rf /tmp/${USERNAME}_mako_rocksdb_shard*

# Kill only target worker processes by executable name.
# Avoid grep/xargs patterns that can match wrapper shells containing process names in argv.
pkill -9 -x dbtest 2>/dev/null || true
pkill -9 -f "$PROC_MATCH" 2>/dev/null || true
sleep 1

cleanup_processes() {
    if [ "$CLEANUP_DONE" -eq 1 ]; then
        return
    fi
    CLEANUP_DONE=1

    for pid in "${SHARD0_PID:-}" "${SHARD1_PID:-}"; do
        if [ -n "$pid" ]; then
            kill "$pid" 2>/dev/null || true
        fi
    done

    sleep 1

    for pid in "${SHARD0_PID:-}" "${SHARD1_PID:-}"; do
        if [ -n "$pid" ]; then
            kill -9 "$pid" 2>/dev/null || true
        fi
    done

    # Last-resort cleanup for lingering test workers.
    pkill -TERM -f "$PROC_MATCH" 2>/dev/null || true
    sleep 1
    pkill -9 -f "$PROC_MATCH" 2>/dev/null || true

    for pid in "${SHARD0_PID:-}" "${SHARD1_PID:-}"; do
        if [ -n "$pid" ]; then
            wait "$pid" 2>/dev/null || true
        fi
    done
}

handle_interrupt() {
    cleanup_processes
    exit 130
}

trap cleanup_processes EXIT
trap handle_interrupt INT TERM

# Ensure logs are writable and fresh so old data cannot produce false positives.
for run_log in "$log_shard0" "$log_shard1"; do
    if ! : > "$run_log"; then
        echo "Error: Cannot write log file '$run_log'."
        echo "Fix file permissions or remove stale files, then retry."
        exit 1
    fi
done

# Start shard 0 in background
echo "Starting shard 0..."
nohup ./${BUILD_DIR:-build}/simpleTransactionRep 2 0 $trd localhost 0 > "$log_shard0" 2>&1 &
SHARD0_PID=$!

# Start shard 1 in background
echo "Starting shard 1..."
nohup ./${BUILD_DIR:-build}/simpleTransactionRep 2 1 $trd localhost 0 > "$log_shard1" 2>&1 &
SHARD1_PID=$!

# Wait for experiments to complete
max_wait="${MAKO_MAX_WAIT_SECONDS:-30}"
if ! [[ "$max_wait" =~ ^[0-9]+$ ]] || [ "$max_wait" -le 0 ]; then
    echo "Warning: MAKO_MAX_WAIT_SECONDS='${max_wait}' is invalid; using default 30s"
    max_wait=30
fi

wait_count=0
benchmark_completed=0
process_exited_early=0
timed_out=0

echo "Waiting for benchmark completion (timeout: ${max_wait}s)..."
while [ "$wait_count" -lt "$max_wait" ]; do
    shard0_done=0
    shard1_done=0
    if [ -f "$log_shard0" ] && grep -q "$completion_marker" "$log_shard0" 2>/dev/null; then
        shard0_done=1
    fi
    if [ -f "$log_shard1" ] && grep -q "$completion_marker" "$log_shard1" 2>/dev/null; then
        shard1_done=1
    fi

    if [ "$shard0_done" -eq 1 ] && [ "$shard1_done" -eq 1 ]; then
        echo "Both benchmarks completed after ${wait_count}s"
        benchmark_completed=1
        sleep 2
        break
    fi

    shard0_alive=1
    shard1_alive=1
    if ! kill -0 "$SHARD0_PID" 2>/dev/null; then
        shard0_alive=0
    fi
    if ! kill -0 "$SHARD1_PID" 2>/dev/null; then
        shard1_alive=0
    fi

    if [ "$shard0_alive" -eq 0 ] || [ "$shard1_alive" -eq 0 ]; then
        sleep 1
        if [ -f "$log_shard0" ] && grep -q "$completion_marker" "$log_shard0" 2>/dev/null; then
            shard0_done=1
        fi
        if [ -f "$log_shard1" ] && grep -q "$completion_marker" "$log_shard1" 2>/dev/null; then
            shard1_done=1
        fi
        if [ "$shard0_done" -eq 1 ] && [ "$shard1_done" -eq 1 ]; then
            echo "Both benchmarks completed after ${wait_count}s (processes exited after writing results)"
            benchmark_completed=1
            sleep 1
            break
        fi
        echo "Process exited unexpectedly before benchmark completion (shard0_alive=$shard0_alive, shard1_alive=$shard1_alive)"
        process_exited_early=1
        break
    fi

    sleep 1
    wait_count=$((wait_count + 1))
    if [ $((wait_count % 10)) -eq 0 ]; then
        echo "  ... waiting (${wait_count}s elapsed, shard0=$shard0_done, shard1=$shard1_done)"
    fi
done

if [ "$wait_count" -ge "$max_wait" ] && [ "$benchmark_completed" -eq 0 ]; then
    echo "Warning: Benchmarks did not complete within ${max_wait}s timeout"
    timed_out=1
fi

# Stop the processes
echo "Stopping shards..."
kill -TERM "$SHARD0_PID" "$SHARD1_PID" 2>/dev/null || true
sleep 2
kill -9 "$SHARD0_PID" "$SHARD1_PID" 2>/dev/null || true
wait "$SHARD0_PID" "$SHARD1_PID" 2>/dev/null || true

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

for log in "$log_shard0" "$log_shard1"; do
    echo ""
    echo "Checking $log:"
    echo "-----------------"

    if [ ! -f "$log" ]; then
        echo "  ✗ Log file not found"
        failed=1
        continue
    fi

    if grep -q "$completion_marker" "$log"; then
        echo "  ✓ Found completion marker: '$completion_marker'"
        grep "$completion_marker" "$log" | tail -1 | sed 's/^/    /'
    else
        echo "  ✗ Completion marker '$completion_marker' not found"
        tail -10 "$log" | sed 's/^/    /'
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
    exit 1
fi
