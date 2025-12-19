#!/bin/bash

# Script to test 2-shard experiments with replication using 4 processes
# Each process handles both shards for one replica role:
#   Process 1: localhost (leader) for both shards
#   Process 2: p1 (follower) for both shards
#   Process 3: p2 (follower) for both shards
#   Process 4: learner for both shards

echo "========================================="
echo "Testing 2-shard replication with 4 processes"
echo "(Minimal process reduction approach)"
echo "========================================="

# Clean up
rm -f nfs_sync_*
rm -f 4proc-*.log
USERNAME=${USER:-unknown}
rm -rf /tmp/${USERNAME}_mako_rocksdb_shard*

trd=6
script_name="$(basename "$0")"
path=$(pwd)/src/mako

# Kill any existing dbtest processes
ps aux | grep -i dbtest | grep -v grep | awk "{print \$2}" | xargs kill -9 2>/dev/null
sleep 1

echo ""
echo "Configuration:"
echo "-----------------"
echo "  Number of threads: $trd"
echo "  Shards: 0, 1 (multi-shard mode)"
echo "  Replicas: localhost, p1, p2, learner"
echo "  Processes: 4 (one per replica role)"
echo ""

# The key difference: use -L 0,1 to run both shards in each process
# And pass Paxos configs for BOTH shards

# Start localhost (leaders for both shards)
echo "Starting localhost (leaders for shards 0,1)..."
nohup ./build/dbtest \
    --num-threads $trd \
    --shard-config $path/config/local-shards2-warehouses$trd.yml \
    -F config/1leader_2followers/paxos${trd}_shardidx0.yml \
    -F config/1leader_2followers/paxos${trd}_shardidx1.yml \
    -F config/occ_paxos.yml \
    -P localhost \
    -L 0,1 \
    --is-replicated \
    > 4proc-localhost.log 2>&1 &
LOCALHOST_PID=$!

sleep 2

# Start p1 (followers for both shards)
echo "Starting p1 (followers for shards 0,1)..."
nohup ./build/dbtest \
    --num-threads $trd \
    --shard-config $path/config/local-shards2-warehouses$trd.yml \
    -F config/1leader_2followers/paxos${trd}_shardidx0.yml \
    -F config/1leader_2followers/paxos${trd}_shardidx1.yml \
    -F config/occ_paxos.yml \
    -P p1 \
    -L 0,1 \
    --is-replicated \
    > 4proc-p1.log 2>&1 &
P1_PID=$!

# Start p2 (followers for both shards)
echo "Starting p2 (followers for shards 0,1)..."
nohup ./build/dbtest \
    --num-threads $trd \
    --shard-config $path/config/local-shards2-warehouses$trd.yml \
    -F config/1leader_2followers/paxos${trd}_shardidx0.yml \
    -F config/1leader_2followers/paxos${trd}_shardidx1.yml \
    -F config/occ_paxos.yml \
    -P p2 \
    -L 0,1 \
    --is-replicated \
    > 4proc-p2.log 2>&1 &
P2_PID=$!

# Start learner (learners for both shards)
echo "Starting learner (learners for shards 0,1)..."
nohup ./build/dbtest \
    --num-threads $trd \
    --shard-config $path/config/local-shards2-warehouses$trd.yml \
    -F config/1leader_2followers/paxos${trd}_shardidx0.yml \
    -F config/1leader_2followers/paxos${trd}_shardidx1.yml \
    -F config/occ_paxos.yml \
    -P learner \
    -L 0,1 \
    --is-replicated \
    > 4proc-learner.log 2>&1 &
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
max_wait=120
wait_count=0

while [ $wait_count -lt $max_wait ]; do
    done_flag=0

    # Check if throughput output appeared
    if [ -f "$log_file" ] && grep -q "agg_persist_throughput" "$log_file" 2>/dev/null; then
        done_flag=1
    fi

    if [ $done_flag -eq 1 ]; then
        echo "Benchmark completed after ${wait_count}s"
        sleep 2
        break
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

if [ $wait_count -ge $max_wait ]; then
    echo "Warning: Benchmark did not complete within ${max_wait}s timeout"
fi

# Graceful shutdown
echo ""
echo "Stopping processes..."
kill -TERM $LOCALHOST_PID $P1_PID $P2_PID $LEARNER_PID 2>/dev/null
sleep 3
kill -9 $LOCALHOST_PID $P1_PID $P2_PID $LEARNER_PID 2>/dev/null
pkill -9 dbtest 2>/dev/null

# Wait for cleanup
sleep 2

echo ""
echo "========================================="
echo "Checking test results..."
echo "========================================="

failed=0

echo ""
echo "Checking 4proc-localhost.log:"
echo "-----------------"

if [ ! -f "4proc-localhost.log" ]; then
    echo "  [X] Log file not found"
    failed=1
else
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
        if grep -q "SiloRuntime.*for shard $shard" "4proc-localhost.log"; then
            echo "  [OK] SiloRuntime created for shard $shard"
        else
            echo "  [X] SiloRuntime not created for shard $shard"
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
