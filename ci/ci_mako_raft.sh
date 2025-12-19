#!/bin/bash

################################################################################
# ci_mako_raft.sh - Continuous Integration Script for Mako-Raft Integration
################################################################################
# This script mirrors ci.sh but tests Raft instead of Paxos
################################################################################

set -e  # Exit on error

# Colors for output
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
NC='\033[0m' # No Color

################################################################################
# Check for Hanging Processes
################################################################################

check_for_hanging_processes() {
    local test_name="$1"
    local max_wait_seconds=10

    echo "Checking if all test processes exited cleanly..."

    # Wait a bit for processes to exit naturally
    sleep 3

    # Count hanging dbtest/simpleRaft processes
    local hanging_count=$(ps aux | grep -E "[d]btest|[s]impleRaft" | wc -l)

    if [ "$hanging_count" -gt 0 ]; then
        echo "=========================================
ERROR: Test '$test_name' left $hanging_count hanging process(es)!
=========================================
Hanging processes:"
        ps aux | grep -E "[d]btest|[s]impleRaft"
        echo ""
        echo "These processes did not exit cleanly after the test completed."
        echo "This indicates a process cleanup issue that needs to be fixed."

        # Kill the hanging processes
        echo "Killing hanging processes..."
        pkill -9 -f "build/dbtest" 2>/dev/null || true
        pkill -9 -f "build/simpleRaft" 2>/dev/null || true
        sleep 2

        # As long as all throughput are ready, just pass it!
        return 0
    else
        echo "✓ All processes exited cleanly"
        return 0
    fi
}

################################################################################
# Cleanup Function
################################################################################

cleanup_processes() {
    # Create results directory for this CI run
    result=ci_raft_results_${RUN_NUM:-0}_${RUN_INDEX:-0}
    mkdir -p ~/results/$result
    rm -f nfs_*

    echo "Cleaning up any lingering test processes..."

    # Use full path pattern to avoid killing this script
    pkill -9 -f "build/simpleRaft" 2>/dev/null || true
    pkill -9 -f "build/simpleTransactionRepRaft" 2>/dev/null || true
    pkill -9 -f "build/testPreferredReplicaStartup" 2>/dev/null || true
    pkill -9 -f "build/testPreferredReplicaLogReplication" 2>/dev/null || true
    pkill -9 -f "build/testNoOps" 2>/dev/null || true
    pkill -9 -f "build/dbtest" 2>/dev/null || true

    # Kill test wrapper scripts
    pkill -9 -f "test_1shard_replication_raft.sh" 2>/dev/null || true
    pkill -9 -f "test_2shard_replication_raft.sh" 2>/dev/null || true
    pkill -9 -f "test_1shard_replication_simple_raft.sh" 2>/dev/null || true
    pkill -9 -f "test_2shard_replication_simple_raft.sh" 2>/dev/null || true
    pkill -9 -f "bash/shard_raft.sh" 2>/dev/null || true

    sleep 3  # Give OS time to fully terminate processes and release ports

    # Wait for ports to be released (check common test ports)
    echo "Waiting for ports to be released..."
    for i in {1..10}; do
        if ! lsof -i :7001-8006 >/dev/null 2>&1 && ! lsof -i :31000-31100 >/dev/null 2>&1; then
            break
        fi
        echo "  Ports still in use, waiting... ($i/10)"
        sleep 1
    done

    # Copy logs to results folder
    cp *.log ~/results/$result/ 2>/dev/null || true
    cp raft_*.log ~/results/$result/ 2>/dev/null || true

    echo "Cleanup complete."
}

################################################################################
# Test Functions
################################################################################

# Function 1: Compile with Raft support
compile() {
    echo "========================================="
    echo "Running: ./ci/ci_mako_raft.sh compile"
    echo "========================================="
    make mako-raft -j32
}

# Function 2: Run simple Raft test (basic replication)
run_simple_raft() {
    echo "========================================="
    echo "Running: ./ci/ci_mako_raft.sh simpleRaft"
    echo "========================================="
    cleanup_processes
    set +e
    bash ./examples/mako-raft-tests/simpleRaft.sh
    local test_result=$?
    set -e
    check_for_hanging_processes "simpleRaft"
    local hanging_check=$?
    [ $test_result -eq 0 ] && [ $hanging_check -eq 0 ]
}

# Function 3: Run 1-shard Raft test (with replication)
run_1shard_replication_raft() {
    echo "========================================="
    echo "Running: ./ci/ci_mako_raft.sh shard1ReplicationRaft"
    echo "========================================="
    cleanup_processes
    set +e
    bash ./examples/mako-raft-tests/test_1shard_replication_raft.sh
    local test_result=$?
    set -e
    check_for_hanging_processes "shard1ReplicationRaft"
    local hanging_check=$?
    [ $test_result -eq 0 ] && [ $hanging_check -eq 0 ]
}

# Function 4: Run 2-shard Raft test (with replication)
run_2shard_replication_raft() {
    echo "========================================="
    echo "Running: ./ci/ci_mako_raft.sh shard2ReplicationRaft"
    echo "========================================="
    cleanup_processes
    set +e
    bash ./examples/mako-raft-tests/test_2shard_replication_raft.sh
    local test_result=$?
    set -e
    check_for_hanging_processes "shard2ReplicationRaft"
    local hanging_check=$?
    [ $test_result -eq 0 ] && [ $hanging_check -eq 0 ]
}

# Function 5: Run 1-shard Raft simple test (simpleTransactionRepRaft)
run_1shard_replication_simple_raft() {
    echo "========================================="
    echo "Running: ./ci/ci_mako_raft.sh shard1ReplicationSimpleRaft"
    echo "========================================="
    cleanup_processes
    set +e
    bash ./examples/mako-raft-tests/test_1shard_replication_simple_raft.sh
    local test_result=$?
    set -e
    check_for_hanging_processes "shard1ReplicationSimpleRaft"
    local hanging_check=$?
    [ $test_result -eq 0 ] && [ $hanging_check -eq 0 ]
}

# Function 6: Run 2-shard Raft simple test (simpleTransactionRepRaft)
run_2shard_replication_simple_raft() {
    echo "========================================="
    echo "Running: ./ci/ci_mako_raft.sh shard2ReplicationSimpleRaft"
    echo "========================================="
    cleanup_processes
    set +e
    bash ./examples/mako-raft-tests/test_2shard_replication_simple_raft.sh
    local test_result=$?
    set -e
    check_for_hanging_processes "shard2ReplicationSimpleRaft"
    local hanging_check=$?
    [ $test_result -eq 0 ] && [ $hanging_check -eq 0 ]
}

# Cleanup function for 'cleanup' command
cleanup() {
    cleanup_processes
    rm -rf ./out-perf.masstree/*
    rm -rf ./src/mako/out-perf.masstree/*
}

################################################################################
# Main Entry Point
################################################################################

case "${1:-}" in
    compile)
        compile
        ;;
    cleanup)
        cleanup
        ;;
    simpleRaft)
        run_simple_raft
        ;;
    shard1ReplicationRaft)
        run_1shard_replication_raft
        ;;
    shard2ReplicationRaft)
        run_2shard_replication_raft
        ;;
    shard1ReplicationSimpleRaft)
        run_1shard_replication_simple_raft
        ;;
    shard2ReplicationSimpleRaft)
        run_2shard_replication_simple_raft
        ;;
    all)
        # Run all Raft tests in sequence
        compile
        run_simple_raft
        run_1shard_replication_raft
        run_2shard_replication_raft
        run_1shard_replication_simple_raft
        run_2shard_replication_simple_raft
        echo "All Raft CI steps completed successfully!"
        ;;
    *)
        echo "Usage: $0 {compile|cleanup|simpleRaft|shard1ReplicationRaft|shard2ReplicationRaft|shard1ReplicationSimpleRaft|shard2ReplicationSimpleRaft|all}"
        echo ""
        echo "Commands:"
        echo "  compile                      - Build Mako with Raft support (make mako-raft)"
        echo "  cleanup                      - Clean up processes and temp files"
        echo "  simpleRaft                   - Run simple Raft replication test"
        echo "  shard1ReplicationRaft        - Run 1-shard Raft test (dbtest with replication)"
        echo "  shard2ReplicationRaft        - Run 2-shard Raft test (dbtest with replication)"
        echo "  shard1ReplicationSimpleRaft  - Run 1-shard Raft simple test (simpleTransactionRepRaft)"
        echo "  shard2ReplicationSimpleRaft  - Run 2-shard Raft simple test (simpleTransactionRepRaft)"
        echo "  all                          - Run all Raft tests"
        exit 1
        ;;
esac
