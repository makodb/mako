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

# Clean up any existing processes
echo "Cleaning up existing processes..."
pkill -9 continuousTransactions 2>/dev/null
pkill -9 dbtest 2>/dev/null
pkill -9 simpleTransactionRep 2>/dev/null
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

# Function to start a shard
start_shard() {
    local shard_id=$1
    local num_workers=$2

    echo "Starting shard $shard_id with $num_workers workers..."

    # Usage: continuousTransactions <nshards> <shardIdx> <nthreads> <paxos_proc_name> [is_replicated]
    nohup $GDB_PREFIX "$MAKO_ROOT/${BUILD_DIR:-build}/continuousTransactions" $SHARDS $shard_id $num_workers localhost 0 > continuous-shard${shard_id}.log 2>&1 &

    echo $!
}

# Start all shards (minimize delay between shard starts)
declare -a PIDS
for ((i=0; i<$SHARDS; i++)); do
    PID=$(start_shard $i $WORKERS)
    PIDS+=($PID)
    echo "  Shard $i started with PID: $PID"
    sleep 0.5  # Minimal delay to avoid overwhelming the system
done

# Give shards time to initialize eRPC servers
echo ""
echo "========================================="
echo "Running continuous transactions..."
echo "========================================="
echo ""

# Wait for the specified duration
echo "Running for $DURATION seconds..."
sleep $DURATION

# Gracefully stop all shards
echo ""
echo "========================================="
echo "Stopping all shards..."
echo "========================================="

for PID in "${PIDS[@]}"; do
    kill -SIGINT $PID 2>/dev/null
done

# Wait a bit for graceful shutdown
sleep 3

# Force kill if still running
for PID in "${PIDS[@]}"; do
    kill -9 $PID 2>/dev/null
done

echo ""
echo "Log files saved to:"
for ((i=0; i<$SHARDS; i++)); do
    echo "  - continuous-shard${i}.log"
done

echo ""
echo "========================================="
echo "Test completed successfully!"
echo "========================================="

# Clean up processes one more time
pkill -9 continuousTransactions 2>/dev/null
sleep 1

exit 0
