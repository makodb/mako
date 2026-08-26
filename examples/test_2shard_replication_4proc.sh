#!/bin/bash

# Script to test 2-shard experiments with replication using 4 processes
# Each process handles both shards for one replica role:
#   Process 1: localhost (leader) for both shards
#   Process 2: p1 (follower) for both shards
#   Process 3: p2 (follower) for both shards
#   Process 4: learner for both shards

# Source common utilities (includes GDB_PREFIX for debugging)
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
source "${SCRIPT_DIR}/../bash/util.sh"

echo "========================================="
echo "Testing 2-shard replication with 4 processes"
echo "(Minimal process reduction approach)"
echo "========================================="

if [ "$GDB_ENABLED" == "1" ]; then
    echo "[GDB] Debug mode enabled"
fi

# Clean up
rm -f nfs_sync_*
rm -f 4proc-*.log
USERNAME=${USER:-unknown}
rm -rf /tmp/${USERNAME}_mako_rocksdb_shard*

trd=${1:-${MAKO_CI_TRD:-6}}
script_name="$(basename "$0")"
path=$(pwd)/src/mako
binary_path="./${BUILD_DIR:-build}/dbtest"
dbtest_match="local-shards2-warehouses${trd}.yml.*--is-replicated"
LOCALHOST_PID=""
P1_PID=""
P2_PID=""
LEARNER_PID=""
STARTED_PROCESSES=0
CLEANUP_DONE=0

cleanup_processes() {
    if [ "$CLEANUP_DONE" -eq 1 ]; then
        return
    fi
    CLEANUP_DONE=1

    if [ "$STARTED_PROCESSES" -eq 0 ]; then
        return
    fi

    echo ""
    echo "Stopping processes..."

    for pid in "$LOCALHOST_PID" "$P1_PID" "$P2_PID" "$LEARNER_PID"; do
        if [ -n "$pid" ] && kill -0 "$pid" 2>/dev/null; then
            kill -TERM "$pid" 2>/dev/null || true
        fi
    done

    sleep 3

    for pid in "$LOCALHOST_PID" "$P1_PID" "$P2_PID" "$LEARNER_PID"; do
        if [ -n "$pid" ] && kill -0 "$pid" 2>/dev/null; then
            kill -9 "$pid" 2>/dev/null || true
        fi
    done

    # Last-resort cleanup for descendant dbtest processes tied to this scenario.
    pkill -TERM -f "$dbtest_match" 2>/dev/null || true
    sleep 1
    pkill -9 -f "$dbtest_match" 2>/dev/null || true

    # Wait for cleanup
    sleep 2
}

handle_interrupt() {
    echo ""
    echo "Received interrupt signal; aborting run."
    exit 130
}

trap cleanup_processes EXIT
trap handle_interrupt INT TERM

if [ ! -x "$binary_path" ]; then
    echo "Error: dbtest binary not found or not executable at '$binary_path'"
    echo "Build it first (for Docker: ./docker_build.sh build), then retry."
    exit 1
fi

# This scenario opens many sockets/files across 4 replicated processes.
# Raise nofile so dbtest/yaml open operations do not fail under default 1024 limits.
target_nofile=65535
hard_nofile=$(ulimit -Hn 2>/dev/null || echo "")
if [ -n "$hard_nofile" ] && [ "$hard_nofile" != "unlimited" ] && [ "$hard_nofile" -lt "$target_nofile" ]; then
    target_nofile="$hard_nofile"
fi
ulimit -n "$target_nofile" 2>/dev/null || true
current_nofile=$(ulimit -n 2>/dev/null || echo "unknown")

# Kill only stale dbtest workers tied to this 4-process replicated scenario.
# Avoid global grep/xargs cleanup that can terminate unrelated shells or workloads.
pkill -9 -f "$dbtest_match" 2>/dev/null || true
sleep 1

echo ""
echo "Configuration:"
echo "-----------------"
echo "  Number of threads: $trd"
echo "  Shards: 0, 1 (multi-shard mode)"
echo "  Replicas: localhost, p1, p2, learner"
echo "  Processes: 4 (one per replica role)"
echo "  Open files limit: $current_nofile"
echo ""

# The key difference: use -L 0,1 to run both shards in each process
# And pass Paxos configs for BOTH shards

# Start localhost (leaders for both shards)
echo "Starting localhost (leaders for shards 0,1)..."
nohup $GDB_PREFIX ./${BUILD_DIR:-build}/dbtest --num-threads $trd --shard-config $path/config/local-shards2-warehouses$trd.yml -F config/1leader_2followers/paxos${trd}_shardidx0.yml -F config/1leader_2followers/paxos${trd}_shardidx1.yml -F config/paxos.yml -P localhost -L 0,1 --is-replicated > 4proc-localhost.log 2>&1 &
LOCALHOST_PID=$!
STARTED_PROCESSES=1

sleep 2

# Start p1 (followers for both shards)
echo "Starting p1 (followers for shards 0,1)..."
nohup $GDB_PREFIX ./${BUILD_DIR:-build}/dbtest --num-threads $trd --shard-config $path/config/local-shards2-warehouses$trd.yml -F config/1leader_2followers/paxos${trd}_shardidx0.yml -F config/1leader_2followers/paxos${trd}_shardidx1.yml -F config/paxos.yml -P p1 -L 0,1 --is-replicated > 4proc-p1.log 2>&1 &
P1_PID=$!

# Start p2 (followers for both shards)
echo "Starting p2 (followers for shards 0,1)..."
nohup $GDB_PREFIX ./${BUILD_DIR:-build}/dbtest --num-threads $trd --shard-config $path/config/local-shards2-warehouses$trd.yml -F config/1leader_2followers/paxos${trd}_shardidx0.yml -F config/1leader_2followers/paxos${trd}_shardidx1.yml -F config/paxos.yml -P p2 -L 0,1 --is-replicated > 4proc-p2.log 2>&1 &
P2_PID=$!

# Start learner (learners for both shards)
echo "Starting learner (learners for shards 0,1)..."
nohup $GDB_PREFIX ./${BUILD_DIR:-build}/dbtest --num-threads $trd --shard-config $path/config/local-shards2-warehouses$trd.yml -F config/1leader_2followers/paxos${trd}_shardidx0.yml -F config/1leader_2followers/paxos${trd}_shardidx1.yml -F config/paxos.yml -P learner -L 0,1 --is-replicated > 4proc-learner.log 2>&1 &
LEARNER_PID=$!

echo ""
echo "Started 4 processes:"
echo "  localhost (PID $LOCALHOST_PID)"
echo "  p1 (PID $P1_PID)"
echo "  p2 (PID $P2_PID)"
echo "  learner (PID $LEARNER_PID)"
echo ""

# Wait for benchmarks to complete
echo "Waiting for benchmarks to complete..."
log_file="4proc-localhost.log"
max_wait="${MAKO_MAX_WAIT_SECONDS:-120}"
if ! [[ "$max_wait" =~ ^[0-9]+$ ]] || [ "$max_wait" -le 0 ]; then
    echo "Warning: MAKO_MAX_WAIT_SECONDS='${max_wait}' is invalid; using default 120s"
    max_wait=120
fi
wait_count=0
benchmark_completed=0
timed_out=0
process_exited_early=0
non_leader_exited=0

while [ "$wait_count" -lt "$max_wait" ]; do
    done_flag=0

    # Check if throughput output appeared
    if [ -f "$log_file" ] && grep -q "agg_persist_throughput" "$log_file" 2>/dev/null; then
        done_flag=1
    fi

    if [ "$done_flag" -eq 1 ]; then
        echo "Benchmark completed after ${wait_count}s"
        benchmark_completed=1
        sleep 2
        break
    fi

    localhost_alive=1
    p1_alive=1
    p2_alive=1
    learner_alive=1
    if ! kill -0 "$LOCALHOST_PID" 2>/dev/null; then
        localhost_alive=0
    fi
    if ! kill -0 "$P1_PID" 2>/dev/null; then
        p1_alive=0
    fi
    if ! kill -0 "$P2_PID" 2>/dev/null; then
        p2_alive=0
    fi
    if ! kill -0 "$LEARNER_PID" 2>/dev/null; then
        learner_alive=0
    fi

    # Leader must stay alive until throughput appears. Follower/learner roles may
    # auto-stop once replication reaches terminal state, so do not fail early on those.
    if [ "$localhost_alive" -eq 0 ]; then
        # Give logs a brief moment to flush before classifying as failure.
        sleep 1
        if [ -f "$log_file" ] && grep -q "agg_persist_throughput" "$log_file" 2>/dev/null; then
            echo "Benchmark completed after ${wait_count}s (processes exited after writing results)"
            benchmark_completed=1
            sleep 1
            break
        fi
        echo "Leader process exited unexpectedly before benchmark completion (localhost_alive=$localhost_alive, p1_alive=$p1_alive, p2_alive=$p2_alive, learner_alive=$learner_alive)"
        process_exited_early=1
        break
    fi

    if [ "$p1_alive" -eq 0 ] || [ "$p2_alive" -eq 0 ] || [ "$learner_alive" -eq 0 ]; then
        if [ "$non_leader_exited" -eq 0 ]; then
            echo "Non-leader process exited while leader is still running; continuing to wait for leader throughput (p1_alive=$p1_alive, p2_alive=$p2_alive, learner_alive=$learner_alive)"
        fi
        non_leader_exited=1
    fi

    sleep 1
    wait_count=$((wait_count + 1))
    if [ $((wait_count % 10)) -eq 0 ]; then
        echo "  ... waiting (${wait_count}s elapsed)"
        # Show recent log output for debugging
        if [ -f "$log_file" ]; then
            tail -2 "$log_file" 2>/dev/null | sed 's/^/    /'
        fi
    fi
done

if [ "$wait_count" -ge "$max_wait" ] && [ "$benchmark_completed" -eq 0 ]; then
    echo "Warning: Benchmark did not complete within ${max_wait}s timeout"
    timed_out=1
fi

cleanup_processes

echo ""
echo "========================================="
echo "Checking test results..."
echo "========================================="

failed=0

if [ "$process_exited_early" -eq 1 ]; then
    echo "  [X] Process exited before benchmark completion"
    failed=1
fi

if [ "$non_leader_exited" -eq 1 ]; then
    echo "  [INFO] One or more non-leader processes exited before leader completion; accepted for this topology"
fi

if [ "$timed_out" -eq 1 ]; then
    echo "  [X] Benchmark timed out before throughput was observed"
    failed=1
fi

echo ""
echo "Checking 4proc-localhost.log:"
echo "-----------------"

if [ ! -f "4proc-localhost.log" ]; then
    echo "  [X] Log file not found"
    failed=1
else
    # Check for TPC-C sharding policy initialization
    if grep -q "TPC-C Sharding: Initialized policy" "4proc-localhost.log"; then
        echo "  [OK] TPC-C sharding policy initialized"
        # Show the initialization line for reference
        grep "TPC-C Sharding: Initialized policy" "4proc-localhost.log" | tail -n 1 | sed 's/^/    /'
    else
        echo "  [X] TPC-C sharding policy not initialized"
        failed=1
    fi

    # Check for multi-shard initialization
    if grep -q "Multi-shard mode" "4proc-localhost.log"; then
        echo "  [OK] Multi-shard mode detected"
        grep "Multi-shard mode" "4proc-localhost.log" | head -1 | sed 's/^/    /'
    else
        echo "  [X] Multi-shard mode not detected"
        failed=1
    fi

    # Check for SiloRuntime creation for both shards
    for shard in 0 1; do
        if grep -q "Assigned shared SiloRuntime.*to shard $shard" "4proc-localhost.log"; then
            echo "  [OK] SiloRuntime assigned for shard $shard"
        else
            echo "  [X] SiloRuntime not assigned for shard $shard"
            failed=1
        fi
    done

    # Check for agg_persist_throughput
    if grep -q "agg_persist_throughput" "4proc-localhost.log"; then
        echo "  [OK] Found 'agg_persist_throughput' keyword"
        grep "agg_persist_throughput" "4proc-localhost.log" | tail -1 | sed 's/^/    /'
    else
        echo "  [X] 'agg_persist_throughput' keyword not found"
        # Show last few lines for debugging
        echo "  Last 10 lines of log:"
        tail -10 "4proc-localhost.log" | sed 's/^/    /'
        failed=1
    fi
fi

echo ""
echo "========================================="
if [ $failed -eq 0 ]; then
    echo "All checks passed!"
    echo "2-shard replication with 4 processes is working!"
    echo "========================================="
    exit 0
else
    echo "Some checks failed!"
    echo "========================================="
    echo ""
    echo "Debug: Check 4proc-*.log files for details"
    echo ""
    echo "Last 20 lines of localhost log:"
    tail -20 4proc-localhost.log 2>/dev/null
    exit 1
fi
