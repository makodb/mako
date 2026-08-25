#!/bin/bash

# Script to test 1-shard experiments with replication
# Each shard should:
# 1. Show "agg_persist_throughput" keyword
# 2. Have NewOrder_remote_abort_ratio < 20%, or N/A when no remote txns occur
# 3. The follower receives END from every partition and replays the leader's
#    log (> MAKO_REPLAY_BATCH_MIN batches; default 0 requires one batch)

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
source "${SCRIPT_DIR}/simple_transaction_rep_port_utils.sh"

echo "========================================="
echo "Testing 1-shard setup with replication"
echo "========================================="

trd=${1:-${MAKO_CI_TRD:-6}}
script_name="$(basename "$0")"
binary_path="./${BUILD_DIR:-build}/dbtest"
SHARD0_LOCALHOST_PID=""
SHARD0_LEARNER_PID=""
SHARD0_P2_PID=""
SHARD0_P1_PID=""
CLEANUP_DONE=0

if [ ! -x "$binary_path" ]; then
    echo "Error: dbtest binary not found or not executable at '$binary_path'"
    echo "Build it first (for Docker: ./docker_build.sh build), then retry."
    exit 1
fi

if ! ensure_paxos_replication_configs "$trd" 1; then
    exit 1
fi

TEMP_CONFIG=$(make_simple_txn_rep_config 1 $trd)
if [ -z "$TEMP_CONFIG" ]; then
    exit 1
fi
export MAKO_CONFIG="$TEMP_CONFIG"
echo "dbtest config: $MAKO_CONFIG"

# Randomize paxos replication ports — see test_2shard_replication.sh.
TEMP_PAXOS_DIR=$(make_paxos_replication_configs 1 "$trd" paxos)
if [ -z "$TEMP_PAXOS_DIR" ]; then
    exit 1
fi
export MAKO_PAXOS_CONFIG_DIR="$TEMP_PAXOS_DIR"
echo "paxos replication config dir: $MAKO_PAXOS_CONFIG_DIR"

cleanup_temp_config() {
    if [ "$CLEANUP_DONE" -eq 1 ]; then
        return
    fi
    CLEANUP_DONE=1

    # Stop any started shard wrappers.
    for pid in "${SHARD0_LOCALHOST_PID:-}" "${SHARD0_LEARNER_PID:-}" "${SHARD0_P2_PID:-}" "${SHARD0_P1_PID:-}"; do
        if [ -n "$pid" ]; then
            kill "$pid" 2>/dev/null || true
        fi
    done

    # Best-effort cleanup for dbtest workers tied to this run's unique temp config.
    # This prevents leaked workers when the script exits early (timeout/Ctrl-C).
    if [ -n "${TEMP_CONFIG:-}" ]; then
        pkill -TERM -f "$TEMP_CONFIG" 2>/dev/null || true
        sleep 1
        pkill -9 -f "$TEMP_CONFIG" 2>/dev/null || true
    fi

    for pid in "${SHARD0_LOCALHOST_PID:-}" "${SHARD0_LEARNER_PID:-}" "${SHARD0_P2_PID:-}" "${SHARD0_P1_PID:-}"; do
        if [ -n "$pid" ]; then
            wait "$pid" 2>/dev/null || true
        fi
    done

    rm -f "$TEMP_CONFIG"
    unset MAKO_CONFIG
    if [ -n "${TEMP_PAXOS_DIR:-}" ]; then
        rm -rf "$TEMP_PAXOS_DIR"
    fi
    unset MAKO_PAXOS_CONFIG_DIR
}

handle_interrupt() {
    cleanup_temp_config
    exit 130
}

trap cleanup_temp_config EXIT
trap handle_interrupt INT TERM

# Kill only dbtest worker processes by executable name.
# Avoid grep/xargs patterns that can match wrapper shells containing "dbtest" in argv.
pkill -9 -x dbtest 2>/dev/null || true
# Clean up old log files
rm -f nfs_sync_*
USERNAME=${USER:-unknown}
rm -rf /tmp/${USERNAME}_mako_rocksdb_shard*

# Start shard 0 in background
echo "Starting shard 0..."
nohup bash bash/shard.sh 1 0 $trd localhost 0 1 > $script_name\_shard0-localhost-$trd.log 2>&1 &
SHARD0_LOCALHOST_PID=$!
nohup bash bash/shard.sh 1 0 $trd learner 0 1 > $script_name\_shard0-learner-$trd.log 2>&1 &
SHARD0_LEARNER_PID=$!
nohup bash bash/shard.sh 1 0 $trd p2 0 1 > $script_name\_shard0-p2-$trd.log 2>&1 &
SHARD0_P2_PID=$!
sleep 1
nohup bash bash/shard.sh 1 0 $trd p1 0 1 > $script_name\_shard0-p1-$trd.log 2>&1 &
SHARD0_P1_PID=$!
sleep 2

# Wait for benchmark to complete (poll for completion marker)
echo "Waiting for benchmark to complete..."
log_file="${script_name}_shard0-localhost-$trd.log"
max_wait="${MAKO_MAX_WAIT_SECONDS:-120}"
if ! [[ "$max_wait" =~ ^[0-9]+$ ]] || [ "$max_wait" -le 0 ]; then
    echo "Warning: MAKO_MAX_WAIT_SECONDS='${max_wait}' is invalid; using default 120s"
    max_wait=120
fi
wait_count=0
benchmark_completed=0
timed_out=0
process_exited_early=0

while [ "$wait_count" -lt "$max_wait" ]; do
    # Check if throughput output appeared (indicates completion)
    if [ -f "$log_file" ] && grep -q "agg_persist_throughput" "$log_file" 2>/dev/null; then
        echo "Benchmark completed after ${wait_count}s"
        benchmark_completed=1
        sleep 2  # Give a moment for final output
        break
    fi

    if ! kill -0 "$SHARD0_LOCALHOST_PID" 2>/dev/null; then
        # Process may exit immediately after writing final metrics.
        sleep 1
        if [ -f "$log_file" ] && grep -q "agg_persist_throughput" "$log_file" 2>/dev/null; then
            echo "Benchmark completed after ${wait_count}s (process exited after writing results)"
            benchmark_completed=1
            sleep 1
            break
        fi
        echo "Shard 0 localhost process exited unexpectedly after ${wait_count}s"
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

# Give the follower a grace window to receive every partition's END and
# demonstrate replay before we kill it. replay_batch volume depends on runner
# scheduling and the leader's batch production, so completion and liveness are
# separate invariants: all ENDs plus positive progress. An explicit
# MAKO_REPLAY_BATCH_MIN can still impose a higher floor for targeted stress runs.
log_p1_drain="${script_name}_shard0-p1-$trd.log"
drain_budget="${MAKO_REPLAY_DRAIN_SECONDS:-240}"
drain_stall_budget="${MAKO_REPLAY_DRAIN_STALL_SECONDS:-20}"
end_wait_budget="${MAKO_REPLAY_END_WAIT_SECONDS:-45}"
if ! [[ "$end_wait_budget" =~ ^[0-9]+$ ]] || [ "$end_wait_budget" -le 0 ]; then
    echo "Warning: MAKO_REPLAY_END_WAIT_SECONDS='${end_wait_budget}' is invalid; using default 45s"
    end_wait_budget=45
fi
# The strict-greater-than check makes the default require at least one batch.
replay_min="${MAKO_REPLAY_BATCH_MIN:-0}"
if ! [[ "$replay_min" =~ ^[0-9]+$ ]]; then
    echo "Warning: MAKO_REPLAY_BATCH_MIN='${replay_min}' is invalid; using default 0"
    replay_min=0
fi
end_marker="follower received all end signals: ${trd}/${trd} ended"
follower_complete=0
last_rb=""
stall=0
for ((i = 0; i < drain_budget; i++)); do
    rb=$(grep "replay_batch:" "$log_p1_drain" 2>/dev/null | tail -1 | sed -n 's/.*replay_batch:\([0-9]*\).*/\1/p')
    all_ends=0
    if grep -Fq "$end_marker" "$log_p1_drain" 2>/dev/null; then
        all_ends=1
    fi
    if [ "$all_ends" -eq 1 ] && [ -n "$rb" ] && [ "$rb" -gt "$replay_min" ]; then
        echo "Follower complete: all ${trd} ENDs received, replay_batch=$rb after ${i}s"
        follower_complete=1
        break
    fi
    if [ "$all_ends" -eq 1 ]; then
        if [ -n "$rb" ] && [ "$rb" != "$last_rb" ]; then
            last_rb="$rb"
            stall=0
        else
            stall=$((stall + 1))
        fi
        if [ "$stall" -ge "$drain_stall_budget" ]; then
            echo "Note: follower replay stalled at ${rb:-none} after all ENDs for ${drain_stall_budget}s (${i}s total)"
            break
        fi
    elif [ "$i" -ge "$end_wait_budget" ]; then
        echo "Note: follower did not receive all ${trd} ENDs within ${end_wait_budget}s"
        break
    fi
    sleep 1
done
if [ "$follower_complete" -eq 0 ] && [ "$i" -ge "$drain_budget" ]; then
    echo "Note: follower did not complete after ${drain_budget}s grace (last replay_batch: ${rb:-none})"
fi

# Graceful shutdown: SIGTERM first
echo "Stopping shards (graceful)..."
pkill -TERM -f "dbtest.*shard-index 0" 2>/dev/null || true
sleep 3

# Force kill any remaining processes
pkill -9 -f "dbtest.*shard-index 0" 2>/dev/null || true
sleep 1

# Original cleanup for good measure
kill $SHARD0_LOCALHOST_PID $SHARD0_LEARNER_PID $SHARD0_P2_PID $SHARD0_P1_PID 2>/dev/null || true
wait $SHARD0_LOCALHOST_PID $SHARD0_LEARNER_PID $SHARD0_P2_PID $SHARD0_P1_PID 2>/dev/null || true

echo ""
echo "========================================="
echo "Checking test results..."
echo "========================================="

failed=0

if [ "$process_exited_early" -eq 1 ]; then
    echo "  ✗ Shard 0 localhost process exited before benchmark completion"
    failed=1
fi

if [ "$timed_out" -eq 1 ]; then
    echo "  ✗ Benchmark timed out before throughput was observed"
    failed=1
fi

# Check each shard's output
{
    i=0
    log="${script_name}_shard${i}-localhost-$trd.log"
    echo ""
    echo "Checking $log:"
    echo "-----------------"
    
    if [ ! -f "$log" ]; then
        echo "  ✗ Log file not found"
        failed=1
        continue
    fi
    
    # Check for agg_persist_throughput keyword
    if grep -q "agg_persist_throughput" "$log"; then
        echo "  ✓ Found 'agg_persist_throughput' keyword"
        # Show the line for reference
        grep "agg_persist_throughput" "$log" | tail -1 | sed 's/^/    /'
    else
        echo "  ✗ 'agg_persist_throughput' keyword not found"
        failed=1
    fi
    
    # Check NewOrder_remote_abort_ratio
    if grep -q "NewOrder_remote_abort_ratio:" "$log"; then
        # Extract the abort ratio value
        abort_ratio=$(grep "NewOrder_remote_abort_ratio:" "$log" | tail -1 | awk '{print $2}')
        
        if [ -z "$abort_ratio" ]; then
            echo "  ✗ Could not extract NewOrder_remote_abort_ratio value"
            failed=1
        else
            # Remove % sign if present and convert to float
            abort_value=$(echo "$abort_ratio" | sed 's/%//')
            
            abort_value_lower=$(echo "$abort_value" | tr '[:upper:]' '[:lower:]')

            # In 1-shard mode there are no remote transactions, so ratio may be nan/-nan.
            if [ "$abort_value_lower" = "nan" ] || [ "$abort_value_lower" = "-nan" ]; then
                echo "  ✓ NewOrder_remote_abort_ratio: $abort_ratio (N/A: no remote transactions in single-shard run)"
            # Check if value is less than 20 using awk (more portable than bc)
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
}

# Check replay_batch counter in shard0-p1.log
echo ""
log_p1="${script_name}_shard0-p1-$trd.log"
echo "Checking $log_p1:"
echo "-----------------"

if [ ! -f "$log_p1" ]; then
    echo "  ✗ $log_p1 file not found"
    failed=1
else
    if grep -Fq "$end_marker" "$log_p1" 2>/dev/null; then
        echo "  ✓ Received END from all $trd partitions"
    else
        echo "  ✗ Did not receive END from all $trd partitions"
        failed=1
    fi

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
            # Positive replay proves the follower consumed leader log data.
            # Batch volume is scheduling-dependent; callers may opt into a
            # higher floor with MAKO_REPLAY_BATCH_MIN.
            if [ "$replay_count" -gt "$replay_min" ]; then
                echo "  ✓ replay_batch: $replay_count (> $replay_min)"
            else
                echo "  ✗ replay_batch: $replay_count (should be > $replay_min)"
                failed=1
            fi
        fi
    fi
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
    echo "Check $script_name\_shard0-localhost-$trd.log and $log_p1 for details"
    echo ""
    echo "Last 10 lines of $script_name\_shard0-localhost-$trd.log:"
    tail -10 $script_name\_shard0-localhost-$trd.log 
    echo ""
    echo "Last 5 lines with 'replay_batch' from $log_p1:"
    grep "replay_batch" $log_p1 | tail -5 2>/dev/null || echo "No replay_batch entries found"
    exit 1
fi
