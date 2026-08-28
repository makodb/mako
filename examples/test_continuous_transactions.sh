#!/bin/bash

# Script to test continuous transaction execution with real-time statistics
# Tests with configurable number of shards and workers

# Source common utilities (includes GDB_PREFIX for debugging)
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
source "${SCRIPT_DIR}/../bash/util.sh"

echo "========================================="
echo "Continuous Transaction Test"
echo "========================================="

if [ "$GDB_ENABLED" == "1" ]; then
    echo "[GDB] Debug mode enabled"
fi

# Default values
DEFAULT_SHARDS=2
DEFAULT_WORKERS=4
DEFAULT_DURATION=30
PROC_MATCH_CONTINUOUS="[/]continuousTransactions( |$)"
CLEANUP_DONE=0

# Parse command line arguments
SHARDS=${1:-$DEFAULT_SHARDS}
WORKERS=${2:-$DEFAULT_WORKERS}
DURATION=${3:-$DEFAULT_DURATION}

echo "Configuration:"
echo "  Shards: $SHARDS"
echo "  Workers per shard: $WORKERS"
echo "  Duration: $DURATION seconds"
echo "  Transaction mix: 70% reads, 30% writes"
echo "  Cross-shard detection: Automatic (based on key hash)"
echo ""

# Start all shards (minimize delay between shard starts)
declare -a PIDS

cleanup_processes() {
    if [ "$CLEANUP_DONE" -eq 1 ]; then
        return
    fi
    CLEANUP_DONE=1

    for pid in "${PIDS[@]}"; do
        if [ -n "$pid" ]; then
            kill -SIGINT "$pid" 2>/dev/null || true
        fi
    done
    sleep 2

    for pid in "${PIDS[@]}"; do
        if [ -n "$pid" ]; then
            kill -9 "$pid" 2>/dev/null || true
        fi
    done

    # Last-resort cleanup for lingering workers.
    pkill -TERM -f "$PROC_MATCH_CONTINUOUS" 2>/dev/null || true
    sleep 1
    pkill -9 -f "$PROC_MATCH_CONTINUOUS" 2>/dev/null || true

    for pid in "${PIDS[@]}"; do
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

# Clean up any existing processes
echo "Cleaning up existing processes..."
pkill -9 -f "$PROC_MATCH_CONTINUOUS" 2>/dev/null || true
pkill -9 -x dbtest 2>/dev/null || true
pkill -9 -f "[/]simpleTransactionRep( |$)" 2>/dev/null || true
# Wait for ports to be released
sleep 2

# Clean up old files
echo "Cleaning up old files..."
rm -f nfs_sync_*
rm -f continuous-shard*.log
USERNAME=${USER:-unknown}
rm -rf /tmp/${USERNAME}_mako_rocksdb_shard*

# Get the script directory and construct path to executable
MAKO_ROOT="$( cd "$SCRIPT_DIR/.." && pwd )"
BINARY_PATH="$MAKO_ROOT/${BUILD_DIR:-build}/continuousTransactions"

if [ ! -x "$BINARY_PATH" ]; then
    echo "Error: continuousTransactions binary not found or not executable at '$BINARY_PATH'."
    echo "Build it first, e.g.:"
    echo "  cmake --build ${BUILD_DIR:-build} --target continuousTransactions"
    echo "For Docker workflows, run this inside the dev container with BUILD_DIR=build_docker."
    exit 1
fi

# Function to start a shard
start_shard() {
    local shard_id=$1
    local num_workers=$2

    echo "Starting shard $shard_id with $num_workers workers..." >&2

    # Usage: continuousTransactions <nshards> <shardIdx> <nthreads> <paxos_proc_name> [is_replicated]
    nohup $GDB_PREFIX "$BINARY_PATH" $SHARDS $shard_id $num_workers localhost 0 > continuous-shard${shard_id}.log 2>&1 &

    echo $!
}

for ((i=0; i<$SHARDS; i++)); do
    PID=$(start_shard $i $WORKERS)
    if ! [[ "$PID" =~ ^[0-9]+$ ]]; then
        echo "Error: Failed to capture a valid PID for shard $i (got '$PID')."
        exit 1
    fi
    PIDS+=($PID)
    echo "  Shard $i started with PID: $PID"
    sleep 0.5  # Minimal delay to avoid overwhelming the system
done

# Give shards time to initialize RPC servers
echo ""
echo "========================================="
echo "Running continuous transactions..."
echo "========================================="
echo ""

# Wait for the specified duration
echo "Running for $DURATION seconds..."
for ((sec=1; sec<=DURATION; sec++)); do
    for PID in "${PIDS[@]}"; do
        if ! kill -0 "$PID" 2>/dev/null; then
            echo "Error: shard process PID $PID exited unexpectedly after ${sec}s."
            echo "Check continuous-shard*.log for details."
            exit 1
        fi
    done
    sleep 1
done

# Gracefully stop all shards
echo ""
echo "========================================="
echo "Stopping all shards..."
echo "========================================="
cleanup_processes

echo ""
echo "Log files saved to:"
for ((i=0; i<$SHARDS; i++)); do
    echo "  - continuous-shard${i}.log"
done

echo ""
echo "========================================="
echo "Test completed successfully!"
echo "========================================="

exit 0
