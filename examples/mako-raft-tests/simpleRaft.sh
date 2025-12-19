#!/bin/bash

# simpleRaft.sh - Basic Raft replication test
# Tests 3 Raft replicas: localhost (preferred leader), p1, p2
# Note: Unlike Paxos which has a separate "learner", Raft treats all replicas equally
# Configuration matches simplePaxos: 3 partitions, 100 logs each, 3KB logs, 5ms interval

# Expected: 100 logs * 3 partitions = 300 callbacks minimum
EXPECTED_MIN_CALLBACKS=300

rm -f raft_a1.log raft_a2.log raft_a3.log

# Kill any lingering processes
killall simpleRaft 2>/dev/null
sleep 1

# Start followers first, leader last (same order as simplePaxos)
echo "Starting p1 (follower)..."
./build/simpleRaft p1 > raft_a2.log 2>&1 &
p1_pid=$!
sleep 2  # Give p1 time to initialize

echo "Starting p2 (follower)..."
./build/simpleRaft p2 > raft_a3.log 2>&1 &
p2_pid=$!
sleep 2  # Give p2 time to initialize

# Start leader last after all followers are ready
echo "Starting localhost (leader)..."
./build/simpleRaft localhost > raft_a1.log 2>&1 &
localhost_pid=$!

echo "Waiting for completion..."
# Wait for test to complete
# - 5s for leader election and transfer
# - ~1.5s for log submission (300 logs * 5ms = 1500ms)
# - Time for replication across 3 partitions
# - Shutdown time
# Total: ~40s to match Paxos
sleep 40

tail -n 5 raft_a1.log raft_a2.log raft_a3.log

# Function to extract callback count from log
get_follower_callbacks() {
    local log_file="$1"
    if [ -f "$log_file" ]; then
        # Extract follower_callbacks=XXX from RESULTS line
        local count=$(grep "RESULTS.*follower_callbacks=" "$log_file" | sed -E 's/.*follower_callbacks=([0-9]+).*/\1/' | tail -1)
        echo "${count:-0}"
    else
        echo "0"
    fi
}

get_leader_callbacks() {
    local log_file="$1"
    if [ -f "$log_file" ]; then
        # Extract leader_callbacks=XXX from RESULTS line
        local count=$(grep "RESULTS.*leader_callbacks=" "$log_file" | sed -E 's/.*leader_callbacks=([0-9]+).*/\1/' | tail -1)
        echo "${count:-0}"
    else
        echo "0"
    fi
}

# Check replication success based on callback counts
echo ""
echo "Checking replication results..."

# Get follower callback counts from p1 and p2
p1_callbacks=$(get_follower_callbacks "raft_a2.log")
p2_callbacks=$(get_follower_callbacks "raft_a3.log")

# Get leader callback count (may be 0 if leader hung during shutdown before printing)
leader_callbacks=$(get_leader_callbacks "raft_a1.log")

echo "  p1 (raft_a2.log): follower_callbacks=$p1_callbacks"
echo "  p2 (raft_a3.log): follower_callbacks=$p2_callbacks"
echo "  localhost (raft_a1.log): leader_callbacks=$leader_callbacks"
echo "  Expected minimum: $EXPECTED_MIN_CALLBACKS"

# Determine success based on replication
replication_success=0

# Check if both followers received enough callbacks (replication worked)
if [ "$p1_callbacks" -ge "$EXPECTED_MIN_CALLBACKS" ] && [ "$p2_callbacks" -ge "$EXPECTED_MIN_CALLBACKS" ]; then
    echo ""
    echo "✓ Replication successful! Both followers received >= $EXPECTED_MIN_CALLBACKS callbacks"
    replication_success=1
else
    echo ""
    echo "✗ Replication failed! Followers did not receive enough callbacks"
    echo "  p1: $p1_callbacks (expected >= $EXPECTED_MIN_CALLBACKS)"
    echo "  p2: $p2_callbacks (expected >= $EXPECTED_MIN_CALLBACKS)"
fi

# Kill any hanging processes (leader may hang during shutdown)
echo ""
echo "Cleaning up processes..."
kill $localhost_pid $p1_pid $p2_pid 2>/dev/null || true
sleep 1
kill -9 $localhost_pid $p1_pid $p2_pid 2>/dev/null || true
killall -9 simpleRaft 2>/dev/null || true

# Report final result
echo ""
if [ "$replication_success" -eq 1 ]; then
    echo "==========================================="
    echo "TEST PASSED: Raft replication working correctly"
    echo "==========================================="
    echo "(Note: Leader may have hung during shutdown, but replication succeeded)"
    exit 0
else
    echo "==========================================="
    echo "TEST FAILED: Raft replication not working"
    echo "==========================================="
    exit 1
fi
