#!/bin/bash

# Script to test 2-shard experiments with RAFT replication
# Each shard should:
# 1. Show "agg_persist_throughput" keyword
# 2. Have NewOrder_remote_abort_ratio < 40%

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
source "${SCRIPT_DIR}/simple_transaction_rep_port_utils.sh"

echo "========================================="
echo "Testing 2-shard setup with RAFT replication"
echo "========================================="

#skill dbtest
# Clean up old log files
#rm -f shard0*.log shard1*.log
rm -f nfs_sync_*
rm -f simple-shard0*.log simple-shard1*.log
USERNAME=${USER:-unknown}
rm -rf /tmp/${USERNAME}_mako_rocksdb_shard*

trd=${1:-${MAKO_CI_TRD:-6}}
export MAKO_RAFT_PREFERRED_GRACE_US="${MAKO_RAFT_PREFERRED_GRACE_US:-30000000}"
export MAKO_RAFT_NONPREFERRED_GRACE_ELECTION_MIN_US="${MAKO_RAFT_NONPREFERRED_GRACE_ELECTION_MIN_US:-5000000}"
export MAKO_RAFT_NONPREFERRED_GRACE_ELECTION_MAX_US="${MAKO_RAFT_NONPREFERRED_GRACE_ELECTION_MAX_US:-10000000}"
script_name="$(basename "$0")"
binary_path="./${BUILD_DIR:-build}/dbtest"
SHARD0_LOCALHOST_PID=""
SHARD0_P2_PID=""
SHARD0_P1_PID=""
SHARD1_LOCALHOST_PID=""
SHARD1_P2_PID=""
SHARD1_P1_PID=""
CLEANUP_DONE=0

if [ ! -x "$binary_path" ]; then
    echo "Error: dbtest binary not found or not executable at '$binary_path'"
    echo "Build it first (for Docker: ./docker_build.sh build), then retry."
    exit 1
fi

TEMP_CONFIG=$(make_simple_txn_rep_config 2 $trd)
if [ -z "$TEMP_CONFIG" ]; then
    exit 1
fi
export MAKO_CONFIG="$TEMP_CONFIG"
echo "dbtest config: $MAKO_CONFIG"

# Randomize raft replication ports — see test_2shard_replication.sh.
TEMP_PAXOS_DIR=$(make_paxos_replication_configs 2 "$trd" raft)
if [ -z "$TEMP_PAXOS_DIR" ]; then
    exit 1
fi
export MAKO_PAXOS_CONFIG_DIR="$TEMP_PAXOS_DIR"
echo "raft replication config dir: $MAKO_PAXOS_CONFIG_DIR"

cleanup_processes() {
    if [ "$CLEANUP_DONE" -eq 1 ]; then
        return
    fi
    CLEANUP_DONE=1

    # Stop started wrapper/leader processes first.
    for pid in "${SHARD0_LOCALHOST_PID:-}" "${SHARD0_P2_PID:-}" "${SHARD0_P1_PID:-}" \
               "${SHARD1_LOCALHOST_PID:-}" "${SHARD1_P2_PID:-}" "${SHARD1_P1_PID:-}"; do
        if [ -n "$pid" ]; then
            kill "$pid" 2>/dev/null || true
        fi
    done

    # Best-effort cleanup for dbtest/shard wrappers tied to this run's config.
    pkill -TERM -f "bash/shard.sh 2 " 2>/dev/null || true
    if [ -n "${TEMP_CONFIG:-}" ]; then
        pkill -TERM -f "$TEMP_CONFIG" 2>/dev/null || true
    else
        pkill -TERM -f "local-shards2-warehouses${trd}\\.yml.*raft" 2>/dev/null || true
    fi
    sleep 1
    pkill -9 -f "bash/shard.sh 2 " 2>/dev/null || true
    if [ -n "${TEMP_CONFIG:-}" ]; then
        pkill -9 -f "$TEMP_CONFIG" 2>/dev/null || true
    else
        pkill -9 -f "local-shards2-warehouses${trd}\\.yml.*raft" 2>/dev/null || true
    fi

    for pid in "${SHARD0_LOCALHOST_PID:-}" "${SHARD0_P2_PID:-}" "${SHARD0_P1_PID:-}" \
               "${SHARD1_LOCALHOST_PID:-}" "${SHARD1_P2_PID:-}" "${SHARD1_P1_PID:-}"; do
        if [ -n "$pid" ]; then
            wait "$pid" 2>/dev/null || true
        fi
    done

    if [ -n "${TEMP_CONFIG:-}" ]; then
        rm -f "$TEMP_CONFIG"
    fi
    unset MAKO_CONFIG
    if [ -n "${TEMP_PAXOS_DIR:-}" ]; then
        rm -rf "$TEMP_PAXOS_DIR"
    fi
    unset MAKO_PAXOS_CONFIG_DIR
}

handle_interrupt() {
    cleanup_processes
    exit 130
}

trap cleanup_processes EXIT
trap handle_interrupt INT TERM

# Determine transport type and create unique log prefix
transport="rrr"
log_prefix="${script_name}_${transport}"

# Kill only dbtest worker processes by executable name.
# Avoid grep/xargs patterns that can match wrapper shells containing "dbtest" in argv.
pkill -9 -x dbtest 2>/dev/null || true
sleep 1
# Start shard 0 in background with RAFT replication (3 replicas, no learner)
echo "Starting shard 0 with Raft..."
nohup bash bash/shard.sh 2 0 $trd localhost 0 1 raft > ${log_prefix}_shard0-localhost.log 2>&1 &
SHARD0_LOCALHOST_PID=$!
nohup bash bash/shard.sh 2 0 $trd p2 0 1 raft > ${log_prefix}_shard0-p2.log 2>&1 &
SHARD0_P2_PID=$!
sleep 1
nohup bash bash/shard.sh 2 0 $trd p1 0 1 raft > ${log_prefix}_shard0-p1.log 2>&1 &
SHARD0_P1_PID=$!

sleep 5

# Start shard 1 in background with RAFT replication (delayed start ensures shard1 stays running while shard0 shuts down)
echo "Starting shard 1 with Raft..."
nohup bash bash/shard.sh 2 1 $trd localhost 0 1 raft > ${log_prefix}_shard1-localhost.log 2>&1 &
SHARD1_LOCALHOST_PID=$!
nohup bash bash/shard.sh 2 1 $trd p2 0 1 raft > ${log_prefix}_shard1-p2.log 2>&1 &
SHARD1_P2_PID=$!
sleep 1
nohup bash bash/shard.sh 2 1 $trd p1 0 1 raft > ${log_prefix}_shard1-p1.log 2>&1 &
SHARD1_P1_PID=$!

# Wait for benchmarks to complete (poll for completion markers)
echo "Waiting for benchmarks to complete..."
log_file0="${log_prefix}_shard0-localhost.log"
log_file1="${log_prefix}_shard1-localhost.log"
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
    shard0_done=0
    shard1_done=0

    # Check if throughput output appeared for each shard
    if [ -f "$log_file0" ] && grep -q "agg_persist_throughput" "$log_file0" 2>/dev/null; then
        shard0_done=1
    fi
    if [ -f "$log_file1" ] && grep -q "agg_persist_throughput" "$log_file1" 2>/dev/null; then
        shard1_done=1
    fi

    if [ "$shard0_done" -eq 1 ] && [ "$shard1_done" -eq 1 ]; then
        echo "Both benchmarks completed after ${wait_count}s"
        benchmark_completed=1
        sleep 2  # Give a moment for final output
        break
    fi

    shard0_alive=1
    shard1_alive=1
    if ! kill -0 "$SHARD0_LOCALHOST_PID" 2>/dev/null; then
        shard0_alive=0
    fi
    if ! kill -0 "$SHARD1_LOCALHOST_PID" 2>/dev/null; then
        shard1_alive=0
    fi

    if { [ "$shard0_alive" -eq 0 ] && [ "$shard0_done" -eq 0 ]; } || \
       { [ "$shard1_alive" -eq 0 ] && [ "$shard1_done" -eq 0 ]; }; then
        # Give logs a brief moment to flush before classifying as failure.
        sleep 1
        if [ -f "$log_file0" ] && grep -q "agg_persist_throughput" "$log_file0" 2>/dev/null; then
            shard0_done=1
        fi
        if [ -f "$log_file1" ] && grep -q "agg_persist_throughput" "$log_file1" 2>/dev/null; then
            shard1_done=1
        fi
        if [ "$shard0_done" -eq 1 ] && [ "$shard1_done" -eq 1 ]; then
            echo "Both benchmarks completed after ${wait_count}s (processes exited after writing results)"
            benchmark_completed=1
            sleep 1
            break
        fi
        echo "Shard process exited unexpectedly before benchmark completion (shard0_alive=$shard0_alive, shard1_alive=$shard1_alive)"
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

# Graceful shutdown: SIGTERM first
echo "Stopping shards (graceful)..."

# First, kill the parent bash scripts to prevent them from respawning dbtest
pkill -TERM -f "bash/shard.sh" 2>/dev/null || true

# Send SIGTERM to all dbtest processes
pkill -TERM dbtest 2>/dev/null || true
sleep 3

# Force kill only if processes remain after graceful shutdown.
if pgrep -f "bash/shard.sh" >/dev/null 2>&1 || pgrep -x dbtest >/dev/null 2>&1; then
    echo "Force killing remaining processes..."
    pkill -9 -f "bash/shard.sh" 2>/dev/null || true
    pkill -9 dbtest 2>/dev/null || true
    killall -9 dbtest 2>/dev/null || true
fi

# Wait for OS to clean up
sleep 2

# Check for and kill any remaining non-zombie dbtest processes.
# Ignore zombie entries here: they cannot be killed and are often unrelated stale entries.
remaining_live=$(ps -eo stat=,pid=,comm= | awk '$3=="dbtest" && $1 !~ /^Z/ {c++} END {print c+0}')

if [ "$remaining_live" -gt 0 ]; then
    echo "WARNING: $remaining_live live dbtest process(es) still present after kill attempt"
    ps -eo stat,pid,ppid,args | awk '$4 ~ /dbtest/ && $1 !~ /^Z/'

    # Get PIDs and kill live processes individually
    pids=$(ps -eo stat=,pid=,comm= | awk '$3=="dbtest" && $1 !~ /^Z/ {print $2}')
    for pid in $pids; do
        echo "Force killing PID $pid"
        kill -9 $pid 2>/dev/null || true
    done

    sleep 1
fi

# Final verification - reap zombie processes by explicitly waiting on child PIDs
# This ensures zombie processes are reaped by their parent (this script)
for pid in $SHARD0_LOCALHOST_PID $SHARD0_P2_PID $SHARD0_P1_PID \
           $SHARD1_LOCALHOST_PID $SHARD1_P2_PID $SHARD1_P1_PID; do
    wait $pid 2>/dev/null || true
done

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
    echo "  ✗ Benchmarks timed out before throughput was observed"
    failed=1
fi

# Check each shard's output
for i in 0 1; do
    log="${log_prefix}_shard${i}-localhost.log"
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

            # Check if value is less than 40 using awk (more portable than bc)
            if awk "BEGIN {exit !($abort_value < 40)}"; then
                echo "  ✓ NewOrder_remote_abort_ratio: $abort_ratio (< 40%)"
            else
                echo "  ✗ NewOrder_remote_abort_ratio: $abort_ratio (>= 40%)"
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
    echo "All checks passed! (Raft replication)"
    echo "========================================="
    exit 0
else
    echo "Some checks failed! (Raft replication)"
    echo "========================================="
    echo ""
    echo "Debug information:"
    echo "Check ${log_prefix}_shard*-localhost.log for details"
    tail -10 ${log_prefix}_shard0-localhost.log
    tail -10 ${log_prefix}_shard1-localhost.log
    exit 1
fi
