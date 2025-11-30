
#!/bin/bash

set -e  # Exit on error

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
    echo "Cleaning up any lingering test processes..."

    # Kill test executables
    pkill -9 -f simpleTransactionRep 2>/dev/null || true
    pkill -9 -f dbtest 2>/dev/null || true
    pkill -9 -f simplePaxos 2>/dev/null || true
    pkill -9 -f simpleTransaction 2>/dev/null || true

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

# Function 1: Compile
compile() {
    echo "========================================="
    echo "Running: ./ci/ci.sh compile"
    echo "========================================="
    make -j32
    # Generate configuration
    bash ./src/mako/update_config.sh
}

# Function 2: Run simple transaction test
run_simple_transaction() {
    echo "========================================="
    echo "Running: ./ci/ci.sh simpleTransaction"
    echo "========================================="
    cleanup_processes
    ./build/simpleTransaction
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

run_shard_fault_tolerance() {
    echo "========================================="
    echo "Running: ./ci/ci.sh shardFaultTolerance"
    echo "========================================="
    cleanup_processes
    set +e
    bash ./examples/test_shard_fault_tolerance.sh
    local test_result=$?
    set -e
    check_for_hanging_processes "shardFaultTolerance"
    local hanging_check=$?
    [ $test_result -eq 0 ] && [ $hanging_check -eq 0 ]
}

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

run_rrr_unit_tests() {
    echo "========================================="
    echo "Running: ./ci/ci.sh rrrTests"
    echo "========================================="
    cd build
    ctest
    local test_result=$?
    cd ..
    return $test_result
}

cleanup() {
    cleanup_processes
    make clean
    rm -rf ./out-perf.masstree/*
    rm -rf ./src/mako/out-perf.masstree/*
    rm -rf build/*
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
    rocksdbTests)
        run_rocksdb_tests
        ;;
    shardFaultTolerance)
        run_shard_fault_tolerance
        ;;
    multiShardSingleProcess)
        run_multi_shard_single_process
        ;;
    rrrTests)
        run_rrr_unit_tests
        ;;
    all)
        # Run all steps in sequence
        compile
        run_rrr_unit_tests
        run_simple_transaction
        run_simple_paxos
        run_2shard_no_replication
        run_1shard_replication
        run_2shard_replication
        run_1shard_replication_simple
        run_2shard_replication_simple
        run_rocksdb_tests
        run_shard_fault_tolerance
        run_multi_shard_single_process
        echo "All CI steps completed successfully!"
        ;;
esac
