#!/bin/bash

# Script to test 2-shard experiments with replication using simpleTransactionRep

# Source common utilities (includes GDB_PREFIX for debugging)
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
source "${SCRIPT_DIR}/../bash/util.sh"
source "${SCRIPT_DIR}/simple_transaction_rep_port_utils.sh"

echo "========================================="
echo "Testing 2-shard setup with replication using simpleTransactionRep"
echo "========================================="

if [ "$GDB_ENABLED" == "1" ]; then
    echo "[GDB] Debug mode enabled"
fi

trd=${1:-6}
binary_path="./${BUILD_DIR:-build}/simpleTransactionRep"
verification_marker="ALL VERIFICATIONS PASSED"
log_s0_localhost="simple-shard0-localhost.log"
log_s0_learner="simple-shard0-learner.log"
log_s0_p2="simple-shard0-p2.log"
log_s0_p1="simple-shard0-p1.log"
log_s1_localhost="simple-shard1-localhost.log"
log_s1_learner="simple-shard1-learner.log"
log_s1_p2="simple-shard1-p2.log"
log_s1_p1="simple-shard1-p1.log"

if [ ! -x "$binary_path" ]; then
    echo "Error: simpleTransactionRep binary not found or not executable at '$binary_path'"
    echo "Build it first (for Docker: ./docker_build.sh build), then retry."
    exit 1
fi

# Clean up old log files
rm -f nfs_sync_*
rm -f "$log_s0_localhost" "$log_s0_learner" "$log_s0_p2" "$log_s0_p1" \
      "$log_s1_localhost" "$log_s1_learner" "$log_s1_p2" "$log_s1_p1"
USERNAME=${USER:-unknown}
rm -rf /tmp/${USERNAME}_mako_rocksdb_shard*

ps aux | grep -i dbtest | awk "{print \$2}" | xargs kill -9 2>/dev/null
ps aux | grep -i simpleTransactionRep | awk "{print \$2}" | xargs kill -9 2>/dev/null
sleep 1

for run_log in "$log_s0_localhost" "$log_s0_learner" "$log_s0_p2" "$log_s0_p1" \
               "$log_s1_localhost" "$log_s1_learner" "$log_s1_p2" "$log_s1_p1"; do
    if ! : > "$run_log"; then
        echo "Error: Cannot write log file '$run_log'."
        echo "Fix file permissions or remove stale files, then retry."
        exit 1
    fi
done

TEMP_CONFIG=$(make_simple_txn_rep_config 2 $trd)
if [ -z "$TEMP_CONFIG" ]; then
    exit 1
fi
export MAKO_CONFIG="$TEMP_CONFIG"
echo "simpleTransactionRep config: $MAKO_CONFIG"

cleanup_temp_config() {
    rm -f "$TEMP_CONFIG"
    unset MAKO_CONFIG
}
trap cleanup_temp_config EXIT

# Start BOTH shards simultaneously to avoid timing issues where shard 0 tries
# to connect to shard 1 before shard 1 is ready
echo "Starting shard 0 and shard 1 simultaneously..."

# Start shard 0 followers first
nohup $GDB_PREFIX ./${BUILD_DIR:-build}/simpleTransactionRep 2 0 $trd learner 1 > "$log_s0_learner" 2>&1 &
PID_S0_LEARNER=$!
nohup $GDB_PREFIX ./${BUILD_DIR:-build}/simpleTransactionRep 2 0 $trd p2 1 > "$log_s0_p2" 2>&1 &
PID_S0_P2=$!

# Start shard 1 followers simultaneously
nohup $GDB_PREFIX ./${BUILD_DIR:-build}/simpleTransactionRep 2 1 $trd learner 1 > "$log_s1_learner" 2>&1 &
PID_S1_LEARNER=$!
nohup $GDB_PREFIX ./${BUILD_DIR:-build}/simpleTransactionRep 2 1 $trd p2 1 > "$log_s1_p2" 2>&1 &
PID_S1_P2=$!

sleep 2

# Start p1 followers
nohup $GDB_PREFIX ./${BUILD_DIR:-build}/simpleTransactionRep 2 0 $trd p1 1 > "$log_s0_p1" 2>&1 &
PID_S0_P1=$!
nohup $GDB_PREFIX ./${BUILD_DIR:-build}/simpleTransactionRep 2 1 $trd p1 1 > "$log_s1_p1" 2>&1 &
PID_S1_P1=$!

sleep 3

# Start leaders simultaneously - they wait 5s for setup before starting tests
nohup $GDB_PREFIX ./${BUILD_DIR:-build}/simpleTransactionRep 2 0 $trd localhost 1 > "$log_s0_localhost" 2>&1 &
PID_S0_LOCALHOST=$!
nohup $GDB_PREFIX ./${BUILD_DIR:-build}/simpleTransactionRep 2 1 $trd localhost 1 > "$log_s1_localhost" 2>&1 &
PID_S1_LOCALHOST=$!

# Wait for experiments to complete (includes setup + verification in simpleTransactionRep)
max_wait="${MAKO_MAX_WAIT_SECONDS:-90}"
if ! [[ "$max_wait" =~ ^[0-9]+$ ]] || [ "$max_wait" -le 0 ]; then
    echo "Warning: MAKO_MAX_WAIT_SECONDS='${max_wait}' is invalid; using default 90s"
    max_wait=90
fi
wait_count=0
benchmark_completed=0
process_exited_early=0
timed_out=0

echo "Waiting for replication verification completion (timeout: ${max_wait}s)..."
while [ "$wait_count" -lt "$max_wait" ]; do
    s0_learner_verified=0
    s0_p2_verified=0
    s0_p1_verified=0
    s1_learner_verified=0
    s1_p2_verified=0
    s1_p1_verified=0
    s0_p1_replay=0
    s1_p1_replay=0

    if [ -f "$log_s0_learner" ] && grep -q "$verification_marker" "$log_s0_learner" 2>/dev/null; then
        s0_learner_verified=1
    fi
    if [ -f "$log_s0_p2" ] && grep -q "$verification_marker" "$log_s0_p2" 2>/dev/null; then
        s0_p2_verified=1
    fi
    if [ -f "$log_s0_p1" ] && grep -q "$verification_marker" "$log_s0_p1" 2>/dev/null; then
        s0_p1_verified=1
    fi
    if [ -f "$log_s1_learner" ] && grep -q "$verification_marker" "$log_s1_learner" 2>/dev/null; then
        s1_learner_verified=1
    fi
    if [ -f "$log_s1_p2" ] && grep -q "$verification_marker" "$log_s1_p2" 2>/dev/null; then
        s1_p2_verified=1
    fi
    if [ -f "$log_s1_p1" ] && grep -q "$verification_marker" "$log_s1_p1" 2>/dev/null; then
        s1_p1_verified=1
    fi
    if [ -f "$log_s0_p1" ] && grep -q "replay_batch:" "$log_s0_p1" 2>/dev/null; then
        s0_p1_replay=1
    fi
    if [ -f "$log_s1_p1" ] && grep -q "replay_batch:" "$log_s1_p1" 2>/dev/null; then
        s1_p1_replay=1
    fi

    if [ "$s0_learner_verified" -eq 1 ] && [ "$s0_p2_verified" -eq 1 ] && [ "$s0_p1_verified" -eq 1 ] && \
       [ "$s1_learner_verified" -eq 1 ] && [ "$s1_p2_verified" -eq 1 ] && [ "$s1_p1_verified" -eq 1 ] && \
       [ "$s0_p1_replay" -eq 1 ] && [ "$s1_p1_replay" -eq 1 ]; then
        echo "Verification markers completed after ${wait_count}s"
        benchmark_completed=1
        sleep 1
        break
    fi

    s0_localhost_alive=1
    s0_learner_alive=1
    s0_p2_alive=1
    s0_p1_alive=1
    s1_localhost_alive=1
    s1_learner_alive=1
    s1_p2_alive=1
    s1_p1_alive=1
    if ! kill -0 "$PID_S0_LOCALHOST" 2>/dev/null; then s0_localhost_alive=0; fi
    if ! kill -0 "$PID_S0_LEARNER" 2>/dev/null; then s0_learner_alive=0; fi
    if ! kill -0 "$PID_S0_P2" 2>/dev/null; then s0_p2_alive=0; fi
    if ! kill -0 "$PID_S0_P1" 2>/dev/null; then s0_p1_alive=0; fi
    if ! kill -0 "$PID_S1_LOCALHOST" 2>/dev/null; then s1_localhost_alive=0; fi
    if ! kill -0 "$PID_S1_LEARNER" 2>/dev/null; then s1_learner_alive=0; fi
    if ! kill -0 "$PID_S1_P2" 2>/dev/null; then s1_p2_alive=0; fi
    if ! kill -0 "$PID_S1_P1" 2>/dev/null; then s1_p1_alive=0; fi

    if [ "$s0_localhost_alive" -eq 0 ] || [ "$s0_learner_alive" -eq 0 ] || [ "$s0_p2_alive" -eq 0 ] || [ "$s0_p1_alive" -eq 0 ] || \
       [ "$s1_localhost_alive" -eq 0 ] || [ "$s1_learner_alive" -eq 0 ] || [ "$s1_p2_alive" -eq 0 ] || [ "$s1_p1_alive" -eq 0 ]; then
        sleep 1
        if [ -f "$log_s0_learner" ] && grep -q "$verification_marker" "$log_s0_learner" 2>/dev/null; then s0_learner_verified=1; fi
        if [ -f "$log_s0_p2" ] && grep -q "$verification_marker" "$log_s0_p2" 2>/dev/null; then s0_p2_verified=1; fi
        if [ -f "$log_s0_p1" ] && grep -q "$verification_marker" "$log_s0_p1" 2>/dev/null; then s0_p1_verified=1; fi
        if [ -f "$log_s1_learner" ] && grep -q "$verification_marker" "$log_s1_learner" 2>/dev/null; then s1_learner_verified=1; fi
        if [ -f "$log_s1_p2" ] && grep -q "$verification_marker" "$log_s1_p2" 2>/dev/null; then s1_p2_verified=1; fi
        if [ -f "$log_s1_p1" ] && grep -q "$verification_marker" "$log_s1_p1" 2>/dev/null; then s1_p1_verified=1; fi
        if [ -f "$log_s0_p1" ] && grep -q "replay_batch:" "$log_s0_p1" 2>/dev/null; then s0_p1_replay=1; fi
        if [ -f "$log_s1_p1" ] && grep -q "replay_batch:" "$log_s1_p1" 2>/dev/null; then s1_p1_replay=1; fi
        if [ "$s0_learner_verified" -eq 1 ] && [ "$s0_p2_verified" -eq 1 ] && [ "$s0_p1_verified" -eq 1 ] && \
           [ "$s1_learner_verified" -eq 1 ] && [ "$s1_p2_verified" -eq 1 ] && [ "$s1_p1_verified" -eq 1 ] && \
           [ "$s0_p1_replay" -eq 1 ] && [ "$s1_p1_replay" -eq 1 ]; then
            echo "Verification markers completed after ${wait_count}s (processes exited after writing results)"
            benchmark_completed=1
            sleep 1
            break
        fi
        echo "Process exited unexpectedly before verification completion (s0_localhost=$s0_localhost_alive, s0_learner=$s0_learner_alive, s0_p2=$s0_p2_alive, s0_p1=$s0_p1_alive, s1_localhost=$s1_localhost_alive, s1_learner=$s1_learner_alive, s1_p2=$s1_p2_alive, s1_p1=$s1_p1_alive)"
        process_exited_early=1
        break
    fi

    sleep 1
    wait_count=$((wait_count + 1))
    if [ $((wait_count % 10)) -eq 0 ]; then
        echo "  ... waiting (${wait_count}s elapsed, s0_lrn=$s0_learner_verified, s0_p2=$s0_p2_verified, s0_p1=$s0_p1_verified, s1_lrn=$s1_learner_verified, s1_p2=$s1_p2_verified, s1_p1=$s1_p1_verified)"
    fi
done

if [ "$wait_count" -ge "$max_wait" ] && [ "$benchmark_completed" -eq 0 ]; then
    echo "Warning: Verification did not complete within ${max_wait}s timeout"
    timed_out=1
fi

# Kill ALL processes from both shards
echo "Stopping shards..."
kill -TERM $PID_S0_LOCALHOST $PID_S0_LEARNER $PID_S0_P2 $PID_S0_P1 \
     $PID_S1_LOCALHOST $PID_S1_LEARNER $PID_S1_P2 $PID_S1_P1 2>/dev/null || true
sleep 2
kill -9 $PID_S0_LOCALHOST $PID_S0_LEARNER $PID_S0_P2 $PID_S0_P1 \
     $PID_S1_LOCALHOST $PID_S1_LEARNER $PID_S1_P2 $PID_S1_P1 2>/dev/null || true
wait $PID_S0_LOCALHOST $PID_S0_LEARNER $PID_S0_P2 $PID_S0_P1 \
     $PID_S1_LOCALHOST $PID_S1_LEARNER $PID_S1_P2 $PID_S1_P1 2>/dev/null || true

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

# Check each shard's output
for i in 0 1; do
    if [ "$i" -eq 0 ]; then
        log="$log_s0_p1"
    else
        log="$log_s1_p1"
    fi
    echo ""
    echo "Checking $log:"
    echo "-----------------"

    if [ ! -f "$log" ]; then
        echo "  ✗ Log file not found"
        failed=1
        continue
    fi

        last_replay_batch=$(grep "replay_batch:" "$log" | tail -1)

    if [ -z "$last_replay_batch" ]; then
        echo "  ✗ No 'replay_batch' keyword found in $log"
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

done

# Check follower logs for data integrity verification
# Note: Leaders (localhost) are the source of data and may have cleanup issues,
# so we only verify followers (learner, p1, p2) which receive replicated data
echo ""
echo "Checking data integrity verification in follower logs:"
echo "-----------------"
for shard in 0 1; do
    for log_suffix in learner p2 p1; do
        if [ "$shard" -eq 0 ] && [ "$log_suffix" = "learner" ]; then log="$log_s0_learner"; fi
        if [ "$shard" -eq 0 ] && [ "$log_suffix" = "p2" ]; then log="$log_s0_p2"; fi
        if [ "$shard" -eq 0 ] && [ "$log_suffix" = "p1" ]; then log="$log_s0_p1"; fi
        if [ "$shard" -eq 1 ] && [ "$log_suffix" = "learner" ]; then log="$log_s1_learner"; fi
        if [ "$shard" -eq 1 ] && [ "$log_suffix" = "p2" ]; then log="$log_s1_p2"; fi
        if [ "$shard" -eq 1 ] && [ "$log_suffix" = "p1" ]; then log="$log_s1_p1"; fi

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
    echo "Check simple-shard0-localhost.log and simple-shard1-localhost.log for details"
    tail -10 "$log_s0_localhost" 2>/dev/null
    tail -10 "$log_s1_localhost" 2>/dev/null
    exit 1
fi
