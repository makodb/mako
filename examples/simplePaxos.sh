#!/bin/bash

# Source common utilities (includes GDB_PREFIX for debugging)
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
source "${SCRIPT_DIR}/../bash/util.sh"

if [ "$GDB_ENABLED" == "1" ]; then
    echo "[GDB] Debug mode enabled"
fi

binary_path="./${BUILD_DIR:-build}/simplePaxos"
startup_wait_per_role="${MAKO_PAXOS_ROLE_STARTUP_WAIT_SECONDS:-5}"
if ! [[ "$startup_wait_per_role" =~ ^[0-9]+$ ]] || [ "$startup_wait_per_role" -le 0 ]; then
    echo "Warning: MAKO_PAXOS_ROLE_STARTUP_WAIT_SECONDS='${startup_wait_per_role}' is invalid; using default 5s"
    startup_wait_per_role=5
fi

if [ ! -x "$binary_path" ]; then
    echo "Error: simplePaxos binary not found or not executable at '$binary_path'"
    echo "Build it first (for Docker: ./docker_build.sh build), then retry."
    exit 1
fi

for run_log in a1.log a2.log a3.log a4.log; do
    if ! : > "$run_log"; then
        echo "Error: Cannot write log file '$run_log'."
        exit 1
    fi
done

kill_lingering_simple_paxos() {
    if command -v pkill >/dev/null 2>&1; then
        pkill -TERM -x simplePaxos 2>/dev/null || true
        sleep 1
        pkill -9 -x simplePaxos 2>/dev/null || true
    elif command -v killall >/dev/null 2>&1; then
        killall -TERM simplePaxos 2>/dev/null || true
        sleep 1
        killall -9 simplePaxos 2>/dev/null || true
    fi
}

# Kill any lingering role processes from previous runs.
kill_lingering_simple_paxos
sleep 1

p1_pid=""
p2_pid=""
learner_pid=""
localhost_pid=""
cleanup_done=0

cleanup_simple_paxos() {
    if [ "$cleanup_done" -eq 1 ]; then
        return
    fi
    cleanup_done=1

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

    # Last-resort cleanup for lingering role workers.
    kill_lingering_simple_paxos

    for pid in "$localhost_pid" "$learner_pid" "$p2_pid" "$p1_pid"; do
        if [ -n "$pid" ]; then
            wait "$pid" 2>/dev/null || true
        fi
    done
}

handle_interrupt() {
    cleanup_simple_paxos
    exit 130
}

trap cleanup_simple_paxos EXIT
trap handle_interrupt INT TERM

log_has_pass() {
    local log_file="$1"
    [ -f "$log_file" ] && grep -E "PASS|\\[32mPASS\\[0m" "$log_file" > /dev/null
}

wait_for_role_startup() {
    local pid="$1"
    local role_name="$2"
    local role_log="$3"
    local waited=0

    while [ "$waited" -lt "$startup_wait_per_role" ]; do
        if ! kill -0 "$pid" 2>/dev/null; then
            echo "Process '$role_name' exited during startup after ${waited}s"
            if [ -f "$role_log" ]; then
                tail -20 "$role_log"
            fi
            return 1
        fi
        sleep 1
        waited=$((waited + 1))
    done

    return 0
}

# Start processes with proper synchronization
echo "Starting p1 (follower)..."
$GDB_PREFIX "$binary_path" p1 > a2.log 2>&1 &
p1_pid=$!
wait_for_role_startup "$p1_pid" "p1" "a2.log" || exit 1

echo "Starting p2 (follower)..."
$GDB_PREFIX "$binary_path" p2 > a3.log 2>&1 &
p2_pid=$!
wait_for_role_startup "$p2_pid" "p2" "a3.log" || exit 1

echo "Starting learner..."
$GDB_PREFIX "$binary_path" learner > a4.log 2>&1 &
learner_pid=$!
wait_for_role_startup "$learner_pid" "learner" "a4.log" || exit 1

# Start leader last after all followers are ready
echo "Starting localhost (leader)..."
$GDB_PREFIX "$binary_path" localhost > a1.log 2>&1 &
localhost_pid=$!
wait_for_role_startup "$localhost_pid" "localhost" "a1.log" || exit 1

echo "Waiting for completion..."
max_wait="${MAKO_MAX_WAIT_SECONDS:-60}"
if ! [[ "$max_wait" =~ ^[0-9]+$ ]] || [ "$max_wait" -le 0 ]; then
    echo "Warning: MAKO_MAX_WAIT_SECONDS='${max_wait}' is invalid; using default 60s"
    max_wait=60
fi
wait_count=0
process_exited_early=0
while [ "$wait_count" -lt "$max_wait" ]; do
    if log_has_pass a1.log && log_has_pass a2.log && log_has_pass a3.log && log_has_pass a4.log; then
        echo "All logs reached PASS after ${wait_count}s"
        break
    fi

    localhost_alive=1
    learner_alive=1
    p2_alive=1
    p1_alive=1
    if ! kill -0 "$localhost_pid" 2>/dev/null; then localhost_alive=0; fi
    if ! kill -0 "$learner_pid" 2>/dev/null; then learner_alive=0; fi
    if ! kill -0 "$p2_pid" 2>/dev/null; then p2_alive=0; fi
    if ! kill -0 "$p1_pid" 2>/dev/null; then p1_alive=0; fi

    if [ "$localhost_alive" -eq 0 ] || [ "$learner_alive" -eq 0 ] || [ "$p2_alive" -eq 0 ] || [ "$p1_alive" -eq 0 ]; then
        sleep 1
        if log_has_pass a1.log && log_has_pass a2.log && log_has_pass a3.log && log_has_pass a4.log; then
            echo "All logs reached PASS after ${wait_count}s (processes exited after writing results)"
            break
        fi
        echo "A simplePaxos role exited before PASS markers were complete (localhost=$localhost_alive learner=$learner_alive p2=$p2_alive p1=$p1_alive)"
        process_exited_early=1
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
if [ "$process_exited_early" -eq 1 ]; then
    echo "Process exit detected before PASS completion ✗"
    failed=1
fi
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
