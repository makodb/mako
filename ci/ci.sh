
#!/bin/bash

set -e  # Exit on error

# Cleanup function: Kill any lingering test processes
cleanup_processes() {
    echo "Cleaning up any lingering test processes..."
    pkill -9 -f simpleTransactionRep 2>/dev/null || true
    pkill -9 -f dbtest 2>/dev/null || true
    pkill -9 -f simplePaxos 2>/dev/null || true
    pkill -9 -f simpleTransaction 2>/dev/null || true
    sleep 1  # Give OS time to release ports
    echo "Cleanup complete."
}

# Function 1: Compile
compile() {
    make -j32
}

# Function 2: Run simple transaction test
run_simple_transaction() {
    cleanup_processes
    ./build/simpleTransaction
}

# Function 3: Run simple Paxos test
run_simple_paxos() {
    cleanup_processes
    bash ./src/mako/update_config.sh
    bash ./examples/simplePaxos.sh
}

# Function 4: Run 2-shard no replication test
run_2shard_no_replication() {
    cleanup_processes
    bash ./examples/test_2shard_no_replication.sh
}

run_1shard_replication() {
    cleanup_processes
    bash ./examples/test_1shard_replication.sh
}

run_2shard_replication() {
    cleanup_processes
    bash ./examples/test_2shard_replication.sh
}

run_1shard_replication_simple() {
    cleanup_processes
    bash ./examples/test_1shard_replication_simple.sh
}

run_2shard_replication_simple() {
    cleanup_processes
    bash ./examples/test_2shard_replication_simple.sh
}

run_rocksdb_tests() {
    cleanup_processes
    bash ./examples/run_rocksdb_test.sh
}

# Main entry point with command parsing
case "${1:-}" in
    compile)
        compile
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
    shard1Replication)
        run_1shard_replication
        ;;
    shard2Replication)
        run_2shard_replication
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
    all)
        # Run all steps in sequence
        compile
        run_simple_transaction
        run_simple_paxos
        run_2shard_no_replication
        run_1shard_replication
        run_2shard_replication
        run_1shard_replication_simple
        run_2shard_replication_simple
        run_rocksdb_tests
        echo "All CI steps completed successfully!"
        ;;
esac
