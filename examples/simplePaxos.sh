#!/bin/bash

# Source common utilities (includes GDB_PREFIX for debugging)
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
source "${SCRIPT_DIR}/../bash/util.sh"

if [ "$GDB_ENABLED" == "1" ]; then
    echo "[GDB] Debug mode enabled"
fi

rm -f a1.log a2.log a3.log a4.log

# Kill any lingering processes
killall simplePaxos 2>/dev/null
sleep 1

p1_pid=""
p2_pid=""
learner_pid=""
localhost_pid=""

cleanup_simple_paxos() {
    for pid in "$localhost_pid" "$learner_pid" "$p2_pid" "$p1_pid"; do
        if [ -n "$pid" ] && kill -0 "$pid" 2>/dev/null; then
            kill "$pid" 2>/dev/null || true
        fi
    done
    sleep 1
    for pid in "$localhost_pid" "$learner_pid" "$p2_pid" "$p1_pid"; do
        if [ -n "$pid" ] && kill -0 "$pid" 2>/dev/null; then
            kill -9 "$pid" 2>/dev/null || true
        fi
    done
}
trap cleanup_simple_paxos EXIT

log_has_pass() {
    local log_file="$1"
    [ -f "$log_file" ] && grep -E "PASS|\\[32mPASS\\[0m" "$log_file" > /dev/null
}

# Start processes with proper synchronization
echo "Starting p1 (follower)..."
$GDB_PREFIX ./${BUILD_DIR:-build}/simplePaxos p1 > a2.log 2>&1 &
p1_pid=$!
sleep 5  # Give p1 time to fully initialize

echo "Starting p2 (follower)..."
$GDB_PREFIX ./${BUILD_DIR:-build}/simplePaxos p2 > a3.log 2>&1 &
p2_pid=$!
sleep 5  # Give p2 time to fully initialize

echo "Starting learner..."
$GDB_PREFIX ./${BUILD_DIR:-build}/simplePaxos learner > a4.log 2>&1 &
learner_pid=$!
sleep 5  # Give learner time to fully initialize

# Start leader last after all followers are ready
echo "Starting localhost (leader)..."
$GDB_PREFIX ./${BUILD_DIR:-build}/simplePaxos localhost > a1.log 2>&1 &
localhost_pid=$!

echo "Waiting for completion..."
max_wait="${MAKO_MAX_WAIT_SECONDS:-60}"
if ! [[ "$max_wait" =~ ^[0-9]+$ ]] || [ "$max_wait" -le 0 ]; then
    echo "Warning: MAKO_MAX_WAIT_SECONDS='${max_wait}' is invalid; using default 60s"
    max_wait=60
fi
wait_count=0
while [ "$wait_count" -lt "$max_wait" ]; do
    if log_has_pass a1.log && log_has_pass a2.log && log_has_pass a3.log && log_has_pass a4.log; then
        echo "All logs reached PASS after ${wait_count}s"
        break
    fi

    sleep 1
    wait_count=$((wait_count + 1))
    if [ $((wait_count % 10)) -eq 0 ]; then
        echo "  ... waiting (${wait_count}s elapsed)"
    fi
done

if [ "$wait_count" -ge "$max_wait" ]; then
    echo "Warning: PASS markers not observed within ${max_wait}s timeout"
fi

tail -n 1 a1.log a2.log a3.log a4.log

# Check if all log files contain PASS keyword (with or without ANSI color codes)
echo ""
echo "Checking test results..."
failed=0
for log in a1.log a2.log a3.log a4.log; do
    if [ -f "$log" ]; then
        # Check for PASS with or without ANSI color codes
        if log_has_pass "$log"; then
            echo "$log: PASS found ✓"
        else
            echo "$log: PASS not found ✗"
            failed=1
        fi
    else
        echo "$log: File not found ✗"
        failed=1
    fi
done

if [ $failed -eq 1 ]; then
    echo "Check failed - not all files contain PASS"
    exit 1
else
    echo "All checks passed!"
fi
