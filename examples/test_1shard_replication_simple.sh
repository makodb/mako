#!/bin/bash

# Script to test 1-shard experiments with replication using simpleTransactionRep

# Source common utilities (includes GDB_PREFIX for debugging)
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
source "${SCRIPT_DIR}/../bash/util.sh"
source "${SCRIPT_DIR}/simple_transaction_rep_port_utils.sh"

echo "========================================="
echo "Testing 1-shard setup with replication using simpleTransactionRep"
echo "========================================="

if [ "$GDB_ENABLED" == "1" ]; then
    echo "[GDB] Debug mode enabled"
fi

trd=${1:-${MAKO_CI_TRD:-6}}
binary_path="./${BUILD_DIR:-build}/simpleTransactionRep"
log_localhost="simple-shard0-localhost.log"
log_learner="simple-shard0-learner.log"
log_p2="simple-shard0-p2.log"
log_p1="simple-shard0-p1.log"
verification_marker="ALL VERIFICATIONS PASSED"
PROC_MATCH="[/]simpleTransactionRep( |$)"
PID_LOCALHOST=""
PID_LEARNER=""
PID_P2=""
PID_P1=""
CLEANUP_DONE=0

if [ ! -x "$binary_path" ]; then
    echo "Error: simpleTransactionRep binary not found or not executable at '$binary_path'"
    echo "Build it first (for Docker: ./docker_build.sh build), then retry."
    exit 1
fi

if ! ensure_paxos_replication_configs "$trd" 1; then
    exit 1
fi

# Kill only target worker processes by executable name.
# Avoid grep/xargs patterns that can match wrapper shells containing process names in argv.
pkill -9 -f "$PROC_MATCH" 2>/dev/null || true
# Clean up old log files
rm -f "$log_localhost" "$log_learner" "$log_p2" "$log_p1" nfs_sync_*
USERNAME=${USER:-unknown}
rm -rf /tmp/${USERNAME}_mako_rocksdb_shard*

for run_log in "$log_localhost" "$log_learner" "$log_p2" "$log_p1"; do
    if ! : > "$run_log"; then
        echo "Error: Cannot write log file '$run_log'."
        echo "Fix file permissions or remove stale files, then retry."
        exit 1
    fi
done

TEMP_CONFIG=$(make_simple_txn_rep_config 1 "$trd")
if [ -z "$TEMP_CONFIG" ]; then
    exit 1
fi
export MAKO_CONFIG="$TEMP_CONFIG"
echo "simpleTransactionRep config: $MAKO_CONFIG"

cleanup_temp_config() {
    if [ "$CLEANUP_DONE" -eq 1 ]; then
        return
    fi
    CLEANUP_DONE=1

    for pid in "${PID_LOCALHOST:-}" "${PID_LEARNER:-}" "${PID_P2:-}" "${PID_P1:-}"; do
        if [ -n "$pid" ]; then
            kill "$pid" 2>/dev/null || true
        fi
    done

    sleep 1

    for pid in "${PID_LOCALHOST:-}" "${PID_LEARNER:-}" "${PID_P2:-}" "${PID_P1:-}"; do
        if [ -n "$pid" ]; then
            kill -9 "$pid" 2>/dev/null || true
        fi
    done

    # Last-resort cleanup for any lingering workers in this shell namespace.
    pkill -TERM -f "$PROC_MATCH" 2>/dev/null || true
    sleep 1
    pkill -9 -f "$PROC_MATCH" 2>/dev/null || true

    for pid in "${PID_LOCALHOST:-}" "${PID_LEARNER:-}" "${PID_P2:-}" "${PID_P1:-}"; do
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

# Start shard 0 in background - capture ALL PIDs
echo "Starting shard 0..."
nohup $GDB_PREFIX ./${BUILD_DIR:-build}/simpleTransactionRep 1 0 "$trd" localhost 1 > "$log_localhost" 2>&1 &
PID_LOCALHOST=$!
nohup $GDB_PREFIX ./${BUILD_DIR:-build}/simpleTransactionRep 1 0 "$trd" learner 1 > "$log_learner" 2>&1 &
PID_LEARNER=$!
nohup $GDB_PREFIX ./${BUILD_DIR:-build}/simpleTransactionRep 1 0 "$trd" p2 1 > "$log_p2" 2>&1 &
PID_P2=$!
sleep 1
nohup $GDB_PREFIX ./${BUILD_DIR:-build}/simpleTransactionRep 1 0 "$trd" p1 1 > "$log_p1" 2>&1 &
PID_P1=$!
sleep 2

# Wait for experiments to complete
max_wait="${MAKO_MAX_WAIT_SECONDS:-40}"
if ! [[ "$max_wait" =~ ^[0-9]+$ ]] || [ "$max_wait" -le 0 ]; then
    echo "Warning: MAKO_MAX_WAIT_SECONDS='${max_wait}' is invalid; using default 40s"
    max_wait=40
fi
wait_count=0
benchmark_completed=0
process_exited_early=0
timed_out=0

echo "Waiting for replication verification completion (timeout: ${max_wait}s)..."
while [ "$wait_count" -lt "$max_wait" ]; do
    learner_verified=0
    p2_verified=0
    p1_verified=0
    p1_replay=0

    if [ -f "$log_learner" ] && grep -q "$verification_marker" "$log_learner" 2>/dev/null; then
        learner_verified=1
    fi
    if [ -f "$log_p2" ] && grep -q "$verification_marker" "$log_p2" 2>/dev/null; then
        p2_verified=1
    fi
    if [ -f "$log_p1" ] && grep -q "$verification_marker" "$log_p1" 2>/dev/null; then
        p1_verified=1
    fi
    if [ -f "$log_p1" ] && grep -q "replay_batch:" "$log_p1" 2>/dev/null; then
        p1_replay=1
    fi

    if [ "$learner_verified" -eq 1 ] && [ "$p2_verified" -eq 1 ] && [ "$p1_verified" -eq 1 ] && [ "$p1_replay" -eq 1 ]; then
        echo "Verification markers completed after ${wait_count}s"
        benchmark_completed=1
        sleep 1
        break
    fi

    localhost_alive=1
    learner_alive=1
    p2_alive=1
    p1_alive=1
    if ! kill -0 "$PID_LOCALHOST" 2>/dev/null; then
        localhost_alive=0
    fi
    if ! kill -0 "$PID_LEARNER" 2>/dev/null; then
        learner_alive=0
    fi
    if ! kill -0 "$PID_P2" 2>/dev/null; then
        p2_alive=0
    fi
    if ! kill -0 "$PID_P1" 2>/dev/null; then
        p1_alive=0
    fi

    if [ "$localhost_alive" -eq 0 ] || [ "$learner_alive" -eq 0 ] || [ "$p2_alive" -eq 0 ] || [ "$p1_alive" -eq 0 ]; then
        sleep 1
        if [ -f "$log_learner" ] && grep -q "$verification_marker" "$log_learner" 2>/dev/null; then
            learner_verified=1
        fi
        if [ -f "$log_p2" ] && grep -q "$verification_marker" "$log_p2" 2>/dev/null; then
            p2_verified=1
        fi
        if [ -f "$log_p1" ] && grep -q "$verification_marker" "$log_p1" 2>/dev/null; then
            p1_verified=1
        fi
        if [ -f "$log_p1" ] && grep -q "replay_batch:" "$log_p1" 2>/dev/null; then
            p1_replay=1
        fi
        if [ "$learner_verified" -eq 1 ] && [ "$p2_verified" -eq 1 ] && [ "$p1_verified" -eq 1 ] && [ "$p1_replay" -eq 1 ]; then
            echo "Verification markers completed after ${wait_count}s (processes exited after writing results)"
            benchmark_completed=1
            sleep 1
            break
        fi
        echo "Process exited unexpectedly before verification completion (localhost_alive=$localhost_alive, learner_alive=$learner_alive, p2_alive=$p2_alive, p1_alive=$p1_alive)"
        process_exited_early=1
        break
    fi

    sleep 1
    wait_count=$((wait_count + 1))
    if [ $((wait_count % 10)) -eq 0 ]; then
        echo "  ... waiting (${wait_count}s elapsed, learner=$learner_verified, p2=$p2_verified, p1=$p1_verified, replay=$p1_replay)"
    fi
done

if [ "$wait_count" -ge "$max_wait" ] && [ "$benchmark_completed" -eq 0 ]; then
    echo "Warning: Verification did not complete within ${max_wait}s timeout"
    timed_out=1
fi

# Kill ALL processes
echo "Stopping shards..."
cleanup_temp_config

echo ""
echo "========================================="
echo "Checking test results..."
echo "========================================="

failed=0

if [ "$process_exited_early" -eq 1 ]; then
    echo "  ✗ Process exited before verification completion"
    failed=1
fi

if [ "$timed_out" -eq 1 ]; then
    echo "  ✗ Verification timed out before expected markers were observed"
    failed=1
fi

# Check replay_batch counter in shard0-p1.log
echo ""
echo "Checking $log_p1:"
echo "-----------------"

if [ ! -f "$log_p1" ]; then
    echo "  ✗ $log_p1 file not found"
    failed=1
else
    # Get the last occurrence of replay_batch
    last_replay_batch=$(grep "replay_batch:" "$log_p1" | tail -1)

    if [ -z "$last_replay_batch" ]; then
        echo "  ✗ No 'replay_batch' keyword found in $log_p1"
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
fi

# Check follower logs for data integrity verification
# Note: Leaders (localhost) are the source of data and may have cleanup issues,
# so we only verify followers (learner, p1, p2) which receive replicated data
echo ""
echo "Checking data integrity verification in follower logs:"
echo "-----------------"
for log_suffix in learner p2 p1; do
    log="simple-shard0-${log_suffix}.log"

    if [ ! -f "$log" ]; then
        echo "  ✗ $log: Log file not found"
        failed=1
        continue
    fi

    # Check for "ALL VERIFICATIONS PASSED" message
    if grep -q "$verification_marker" "$log"; then
        echo "  ✓ $log: Data integrity verified"
    else
        echo "  ✗ $log: Data integrity verification FAILED or not found"
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
    echo "Check simple-shard0-localhost.log and simple-shard0-p1.log for details"
    echo ""
    echo "Last 10 lines of simple-shard0-localhost.log:"
    tail -10 simple-shard0-localhost.log
    echo ""
    echo "Last 5 lines with 'replay_batch' from simple-shard0-p1.log:"
    grep "replay_batch" simple-shard0-p1.log | tail -5 2>/dev/null || echo "No replay_batch entries found"
    exit 1
fi
