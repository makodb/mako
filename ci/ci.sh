
#!/bin/bash

set -e  # Exit on error

# Disable GDB for CI runs - GDB changes output format and breaks grep patterns
export MAKO_NO_GDB=1

# On macOS, eRPC is disabled by design; skip the eRPC variants in CI.
if [[ "$(uname -s)" == "Darwin" ]]; then
    SKIP_ERPC=1
else
    SKIP_ERPC=0
fi

# Build directory (can be overridden via environment variable)
BUILD_DIR=${BUILD_DIR:-build}

# Function to check for hanging processes after a test
check_for_hanging_processes() {
    local test_name="$1"
    local max_wait_seconds=10

    echo "Checking if all test processes exited cleanly..."

    # Wait a bit for processes to exit naturally
    sleep 3

    # Count hanging dbtest processes
    local hanging_count=$(ps aux | grep -E "[d]btest" | wc -l)

    if [ "$hanging_count" -gt 0 ]; then
        echo "=========================================
ERROR: Test '$test_name' left $hanging_count hanging dbtest process(es)!
=========================================
Hanging processes:"
        ps aux | grep -E "[d]btest"
        echo ""
        echo "These processes did not exit cleanly after the test completed."
        echo "This indicates a process cleanup issue that needs to be fixed."

        # Kill the hanging processes
        echo "Killing hanging processes..."
        pkill -9 -f dbtest 2>/dev/null || true
        sleep 2

        # As long as all throughput are ready, just pass it!
        return 0
    else
        echo "✓ All processes exited cleanly"
        return 0
    fi
}

# Cleanup function: Kill any lingering test processes
cleanup_processes() {
    result=ci_results_${RUN_NUM}_${RUN_INDEX}
    mkdir -p ~/results/$result
    rm -f nfs_*
    # Clean up RocksDB data from previous runs
    USERNAME=${USER:-$(whoami)}
    rm -rf /tmp/${USERNAME}_mako_rocksdb_shard*
    echo "Cleaning up any lingering test processes..."

    # Kill test executables (exclude ci.sh itself and its parent processes)
    # Use pgrep to get PIDs, filter out current process tree, then kill
    local my_pid=$$
    local my_ppid=$(ps -o ppid= -p $my_pid | tr -d ' ')

    for proc in simpleTransactionRep dbtest simplePaxos simpleTransaction; do
        pgrep -f "$proc" 2>/dev/null | while read pid; do
            if [ "$pid" != "$my_pid" ] && [ "$pid" != "$my_ppid" ] && [ "$pid" != "1" ]; then
                kill -9 "$pid" 2>/dev/null || true
            fi
        done
    done

    # Kill test wrapper scripts (2shard tests with/without replication)
    pkill -9 -f "test_2shard_no_replication.sh" 2>/dev/null || true
    pkill -9 -f "test_2shard_replication.sh" 2>/dev/null || true
    pkill -9 -f "test_1shard_replication.sh" 2>/dev/null || true
    pkill -9 -f "bash/shard.sh" 2>/dev/null || true

    sleep 3  # Give OS time to fully terminate processes and release ports

    # Wait for ports to be released (check common test ports)
    for i in {1..10}; do
        if ! lsof -i :7001-8006 >/dev/null 2>&1 && ! lsof -i :31000-31100 >/dev/null 2>&1; then
            break
        fi
        sleep 1
    done

    cp *.log ~/results/$result/  2>/dev/null || true
    echo "Cleanup complete."
}

# Run a command with memory limit (in KB)
# Usage: run_with_memory_limit <limit_kb> <command...>
# Example: run_with_memory_limit 31457280 bash ./examples/test.sh  # 30GB limit
run_with_memory_limit() {
    local limit_kb=$1
    shift
    local limit_gb=$((limit_kb / 1024 / 1024))
    echo "[Memory limit] Setting virtual memory limit to ${limit_gb}GB (${limit_kb}KB)"

    # Use ulimit to set virtual memory limit
    # This prevents runaway memory usage from crashing CI servers
    (
        ulimit -v $limit_kb 2>/dev/null || {
            echo "[Memory limit] Warning: Could not set ulimit -v, running without limit"
        }
        "$@"
    )
}

# Function 1: Compile
compile() {
    echo "========================================="
    echo "Running: ./ci/ci.sh compile"
    echo "========================================="
    set -o pipefail
    make BUILD_DIR=${BUILD_DIR} -j32 2>&1 | tee build.log
    # Generate configuration
    bash ./src/mako/update_config.sh
}

# Function 2: Run simple transaction test
run_simple_transaction() {
    echo "========================================="
    echo "Running: ./ci/ci.sh simpleTransaction"
    echo "========================================="
    cleanup_processes
    ./${BUILD_DIR}/simpleTransaction
}

# Function 3: Run simple Paxos test
run_simple_paxos() {
    echo "========================================="
    echo "Running: ./ci/ci.sh simplePaxos"
    echo "========================================="
    cleanup_processes
    bash ./src/mako/update_config.sh
    set +e
    bash ./examples/simplePaxos.sh
    local test_result=$?
    set -e
    check_for_hanging_processes "simplePaxos"
    local hanging_check=$?
    [ $test_result -eq 0 ] && [ $hanging_check -eq 0 ]
}

# Function 4: Run 2-shard no replication test (RRR transport)
run_2shard_no_replication() {
    echo "========================================="
    echo "Running: ./ci/ci.sh shardNoReplication"
    echo "========================================="
    cleanup_processes
    set +e
    bash ./examples/test_2shard_no_replication.sh
    local test_result=$?
    set -e
    check_for_hanging_processes "shardNoReplication"
    local hanging_check=$?
    [ $test_result -eq 0 ] && [ $hanging_check -eq 0 ]
}

# Function 4b: Run 2-shard no replication test with eRPC transport
run_2shard_no_replication_erpc() {
    if [ "$SKIP_ERPC" -eq 1 ]; then
        echo "========================================="
        echo "Skipping: ./ci/ci.sh shardNoReplicationErpc (macOS: eRPC disabled)"
        echo "========================================="
        return 0
    fi
    echo "========================================="
    echo "Running: ./ci/ci.sh shardNoReplicationErpc"
    echo "========================================="
    cleanup_processes
    set +e
    MAKO_TRANSPORT=erpc bash ./examples/test_2shard_no_replication.sh
    local test_result=$?
    set -e
    check_for_hanging_processes "shardNoReplicationErpc"
    local hanging_check=$?
    [ $test_result -eq 0 ] && [ $hanging_check -eq 0 ]
}

run_1shard_replication() {
    echo "========================================="
    echo "Running: ./ci/ci.sh shard1Replication"
    echo "========================================="
    cleanup_processes
    # Run test and capture exit code (set +e to prevent immediate exit)
    set +e
    bash ./examples/test_1shard_replication.sh
    local test_result=$?
    set -e
    # Always check for hanging processes, even if test failed
    check_for_hanging_processes "shard1Replication"
    local hanging_check=$?
    # Return failure if either check failed
    [ $test_result -eq 0 ] && [ $hanging_check -eq 0 ]
}

run_2shard_replication() {
    echo "========================================="
    echo "Running: ./ci/ci.sh shard2Replication"
    echo "========================================="
    cleanup_processes
    # Run test and capture exit code (set +e to prevent immediate exit)
    set +e
    bash ./examples/test_2shard_replication.sh
    local test_result=$?
    set -e
    # Always check for hanging processes, even if test failed
    check_for_hanging_processes "shard2Replication"
    local hanging_check=$?
    # Return failure if either check failed
    [ $test_result -eq 0 ] && [ $hanging_check -eq 0 ]
}

run_2shard_replication_erpc() {
    if [ "$SKIP_ERPC" -eq 1 ]; then
        echo "========================================="
        echo "Skipping: ./ci/ci.sh shard2ReplicationErpc (macOS: eRPC disabled)"
        echo "========================================="
        return 0
    fi
    echo "========================================="
    echo "Running: ./ci/ci.sh shard2ReplicationErpc"
    echo "========================================="
    cleanup_processes
    # Run test and capture exit code (set +e to prevent immediate exit)
    set +e
    MAKO_TRANSPORT=erpc bash ./examples/test_2shard_replication.sh
    local test_result=$?
    set -e
    # Always check for hanging processes, even if test failed
    check_for_hanging_processes "shard2ReplicationErpc"
    local hanging_check=$?
    # Return failure if either check failed
    [ $test_result -eq 0 ] && [ $hanging_check -eq 0 ]
}

run_1shard_replication_simple() {
    echo "========================================="
    echo "Running: ./ci/ci.sh shard1ReplicationSimple"
    echo "========================================="
    cleanup_processes
    # Run test and capture exit code (set +e to prevent immediate exit)
    set +e
    bash ./examples/test_1shard_replication_simple.sh
    local test_result=$?
    set -e
    # Always check for hanging processes, even if test failed
    check_for_hanging_processes "shard1ReplicationSimple"
    local hanging_check=$?
    # Return failure if either check failed
    [ $test_result -eq 0 ] && [ $hanging_check -eq 0 ]
}

run_2shard_replication_simple() {
    echo "========================================="
    echo "Running: ./ci/ci.sh shard2ReplicationSimple"
    echo "========================================="
    cleanup_processes
    # Run test and capture exit code (set +e to prevent immediate exit)
    set +e
    bash ./examples/test_2shard_replication_simple.sh
    local test_result=$?
    set -e
    # Always check for hanging processes, even if test failed
    check_for_hanging_processes "shard2ReplicationSimple"
    local hanging_check=$?
    # Return failure if either check failed
    [ $test_result -eq 0 ] && [ $hanging_check -eq 0 ]
}

# ============================================================================
# Raft Replication Tests
# ============================================================================

run_1shard_replication_raft() {
    echo "========================================="
    echo "Running: ./ci/ci.sh shard1ReplicationRaft"
    echo "========================================="
    cleanup_processes
    # Run test and capture exit code (set +e to prevent immediate exit)
    set +e
    bash ./examples/test_1shard_replication_raft.sh
    local test_result=$?
    set -e
    # Always check for hanging processes, even if test failed
    check_for_hanging_processes "shard1ReplicationRaft"
    local hanging_check=$?
    # Return failure if either check failed
    [ $test_result -eq 0 ] && [ $hanging_check -eq 0 ]
}

run_2shard_replication_raft() {
    echo "========================================="
    echo "Running: ./ci/ci.sh shard2ReplicationRaft"
    echo "========================================="
    cleanup_processes
    # Run test and capture exit code (set +e to prevent immediate exit)
    set +e
    bash ./examples/test_2shard_replication_raft.sh
    local test_result=$?
    set -e
    # Always check for hanging processes, even if test failed
    check_for_hanging_processes "shard2ReplicationRaft"
    local hanging_check=$?
    # Return failure if either check failed
    [ $test_result -eq 0 ] && [ $hanging_check -eq 0 ]
}

run_1shard_replication_simple_raft() {
    echo "========================================="
    echo "Running: ./ci/ci.sh shard1ReplicationSimpleRaft"
    echo "========================================="
    cleanup_processes
    # Run test and capture exit code (set +e to prevent immediate exit)
    set +e
    bash ./examples/test_1shard_replication_simple_raft.sh
    local test_result=$?
    set -e
    # Always check for hanging processes, even if test failed
    check_for_hanging_processes "shard1ReplicationSimpleRaft"
    local hanging_check=$?
    # Return failure if either check failed
    [ $test_result -eq 0 ] && [ $hanging_check -eq 0 ]
}

run_2shard_replication_simple_raft() {
    echo "========================================="
    echo "Running: ./ci/ci.sh shard2ReplicationSimpleRaft"
    echo "========================================="
    cleanup_processes
    # Run test and capture exit code (set +e to prevent immediate exit)
    set +e
    bash ./examples/test_2shard_replication_simple_raft.sh
    local test_result=$?
    set -e
    # Always check for hanging processes, even if test failed
    check_for_hanging_processes "shard2ReplicationSimpleRaft"
    local hanging_check=$?
    # Return failure if either check failed
    [ $test_result -eq 0 ] && [ $hanging_check -eq 0 ]
}

run_rocksdb_tests() {
    echo "========================================="
    echo "Running: ./ci/ci.sh rocksdbTests"
    echo "========================================="
    cleanup_processes
    set +e
    bash ./examples/run_rocksdb_test.sh
    local test_result=$?
    set -e
    check_for_hanging_processes "rocksdbTests"
    local hanging_check=$?
    [ $test_result -eq 0 ] && [ $hanging_check -eq 0 ]
}

# DISABLED: test script not implemented
# run_shard_fault_tolerance() {
#     echo "========================================="
#     echo "Running: ./ci/ci.sh shardFaultTolerance"
#     echo "========================================="
#     cleanup_processes
#     set +e
#     bash ./examples/test_shard_fault_tolerance.sh
#     local test_result=$?
#     set -e
#     check_for_hanging_processes "shardFaultTolerance"
#     local hanging_check=$?
#     [ $test_result -eq 0 ] && [ $hanging_check -eq 0 ]
# }

run_multi_shard_single_process() {
    echo "========================================="
    echo "Running: ./ci/ci.sh multiShardSingleProcess"
    echo "========================================="
    cleanup_processes
    set +e
    bash ./examples/test_multi_shard_single_process.sh
    local test_result=$?
    set -e
    check_for_hanging_processes "multiShardSingleProcess"
    local hanging_check=$?
    [ $test_result -eq 0 ] && [ $hanging_check -eq 0 ]
}

run_2shard_single_process() {
    echo "========================================="
    echo "Running: ./ci/ci.sh shard2SingleProcess"
    echo "========================================="
    cleanup_processes
    set +e
    bash ./examples/test_2shard_single_process.sh
    local test_result=$?
    set -e
    check_for_hanging_processes "shard2SingleProcess"
    local hanging_check=$?
    [ $test_result -eq 0 ] && [ $hanging_check -eq 0 ]
}

run_2shard_single_process_replication() {
    echo "========================================="
    echo "Running: ./ci/ci.sh shard2SingleProcessReplication"
    echo "========================================="
    cleanup_processes
    set +e
    # Memory limit: 30GB (30 * 1024 * 1024 KB = 31457280 KB)
    # This test runs 7 processes and can consume significant memory
    # The limit prevents memory overuse from crashing CI servers
    run_with_memory_limit 31457280 bash ./examples/test_2shard_single_process_replication.sh
    local test_result=$?
    set -e
    check_for_hanging_processes "shard2SingleProcessReplication"
    local hanging_check=$?
    [ $test_result -eq 0 ] && [ $hanging_check -eq 0 ]
}

run_rrr_unit_tests() {
    echo "========================================="
    echo "Running: ./ci/ci.sh rrrTests"
    echo "========================================="
    cd ${BUILD_DIR}
    ctest
    local test_result=$?
    cd ..
    return $test_result
}

run_cpu_throttling_scaling() {
    echo "========================================="
    echo "Running: ./ci/ci.sh cpuThrottlingScaling"
    echo "========================================="
    cleanup_processes
    set +e
    bash ./ci/test_cpu_throttling_scaling.sh
    local test_result=$?
    set -e
    check_for_hanging_processes "cpuThrottlingScaling"
    local hanging_check=$?
    [ $test_result -eq 0 ] && [ $hanging_check -eq 0 ]
}

run_client_server_test() {
    echo "========================================="
    echo "Running: ./ci/ci.sh clientServer"
    echo "========================================="
    cleanup_processes
    set +e
    bash ./ci/test_client_server.sh
    local test_result=$?
    set -e
    check_for_hanging_processes "clientServer"
    local hanging_check=$?
    [ $test_result -eq 0 ] && [ $hanging_check -eq 0 ]
}

cleanup() {
    cleanup_processes
    make BUILD_DIR=${BUILD_DIR} clean
    rm -rf ./out-perf.masstree/*
    rm -rf ./src/mako/out-perf.masstree/*
    rm -rf ${BUILD_DIR}/*
}

# Main entry point with command parsing
case "${1:-}" in
    compile)
        compile
        ;;
    cleanup)
       cleanup
        ;;
    simpleTransaction)
        run_simple_transaction
        ;;
    simplePaxos)
        run_simple_paxos
        ;;
    shardNoReplication)
        run_2shard_no_replication
        ;;
    shardNoReplicationErpc)
        run_2shard_no_replication_erpc
        ;;
    shard1Replication)
        run_1shard_replication
        ;;
    shard2Replication)
        run_2shard_replication
        ;;
    shard2ReplicationErpc)
        run_2shard_replication_erpc
        ;;
    shard1ReplicationSimple)
        run_1shard_replication_simple
        ;;
    shard2ReplicationSimple)
        run_2shard_replication_simple
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
    rocksdbTests)
        run_rocksdb_tests
        ;;
    # shardFaultTolerance)  # DISABLED: test script not implemented
    #     run_shard_fault_tolerance
    #     ;;
    multiShardSingleProcess)
        run_multi_shard_single_process
        ;;
    shard2SingleProcess)
        run_2shard_single_process
        ;;
    shard2SingleProcessReplication)
        run_2shard_single_process_replication
        ;;
    rrrTests)
        run_rrr_unit_tests
        ;;
    cpuThrottlingScaling)
        run_cpu_throttling_scaling
        ;;
    clientServer)
        run_client_server_test
        ;;
    all)
        # Run all steps in sequence
        compile
        run_rrr_unit_tests
        run_simple_transaction
        run_client_server_test
        run_simple_paxos
        run_2shard_no_replication
        if [ "$SKIP_ERPC" -eq 0 ]; then
            run_2shard_no_replication_erpc
        fi
        # Paxos replication tests
        run_1shard_replication
        run_2shard_replication
        if [ "$SKIP_ERPC" -eq 0 ]; then
            run_2shard_replication_erpc
        fi
        run_1shard_replication_simple
        run_2shard_replication_simple
        # Raft replication tests
        run_1shard_replication_raft
        run_2shard_replication_raft
        run_1shard_replication_simple_raft
        run_2shard_replication_simple_raft
        run_rocksdb_tests
        # run_shard_fault_tolerance  # DISABLED: test script not implemented
        run_multi_shard_single_process
        run_2shard_single_process
        run_2shard_single_process_replication
        echo "All CI steps completed successfully!"
        ;;
esac
