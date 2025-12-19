#!/bin/bash
# Script to test RocksDB persistence implementation
set -e  # Exit on error

echo "=== RocksDB Persistence Test Script ==="
echo ""

# Track test failures
FAILED_TESTS=0

# Get the project root (parent of examples)
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(dirname "$SCRIPT_DIR")"
cd "$PROJECT_ROOT"

# Clean test directories
echo "1. Cleaning test RocksDB directories..."
# Get current username for cleanup
USERNAME=${USER:-unknown}
rm -rf /tmp/${USERNAME}_*

# Run test
echo "2. Running RocksDB persistence tests..."
if ./build/test_rocksdb_persistence > /tmp/${USERNAME}_rocksdb_test_output.txt 2>&1; then
    echo "   ✓ Basic persistence tests passed"
    echo ""
    echo "Test output summary:"
    grep "===" /tmp/${USERNAME}_rocksdb_test_output.txt
    echo ""
    # Show performance metrics
    grep -E "Throughput:|Time taken:" /tmp/${USERNAME}_rocksdb_test_output.txt | head -3
else
    echo "   ✗ Basic persistence tests failed"
    cat /tmp/${USERNAME}_rocksdb_test_output.txt
    exit 1
fi

# Run callback demo test
echo ""
echo "3. Running callback demonstration test..."
if ./build/test_callback_demo > /tmp/${USERNAME}_callback_demo_output.txt 2>&1; then
    echo "   ✓ Callback demo passed"
    echo ""
    echo "Callback demo output:"
    grep -E "===|Total|Persisted:|Failed:" /tmp/${USERNAME}_callback_demo_output.txt
else
    echo "   ✗ Callback demo failed"
    cat /tmp/${USERNAME}_callback_demo_output.txt
    exit 1
fi

# Run ordered callbacks test
echo ""
echo "4. Running ordered callbacks test..."
if ./build/test_ordered_callbacks > /tmp/${USERNAME}_ordered_callbacks_output.txt 2>&1; then
    echo "   ✓ Ordered callbacks test passed"
    echo ""
    echo "Ordered callbacks output:"
    grep -E "===|✓|✗|ERROR" /tmp/${USERNAME}_ordered_callbacks_output.txt
else
    echo "   ✗ Ordered callbacks test failed"
    cat /tmp/${USERNAME}_ordered_callbacks_output.txt
    exit 1
fi

# Run partitioned queues test
echo ""
echo "5. Running partitioned queues test..."
if ./build/test_partitioned_queues > /tmp/${USERNAME}_partitioned_queues_output.txt 2>&1; then
    echo "   ✓ Partitioned queues test passed"
    echo ""
    echo "Partitioned queues output:"
    grep -E "===|✓|✗|Throughput:|Worker" /tmp/${USERNAME}_partitioned_queues_output.txt
else
    echo "   ✗ Partitioned queues test failed"
    cat /tmp/${USERNAME}_partitioned_queues_output.txt
    exit 1
fi

# Run stress test
echo ""
echo "6. Running complex stress test (20 threads, 10 partitions, mixed load)..."
if ./build/test_stress_partitioned_queues > /tmp/${USERNAME}_stress_test_output.txt 2>&1; then
    # Check if test actually passed by looking for FAILURE in output
    if grep -q "FAILURE" /tmp/${USERNAME}_stress_test_output.txt; then
        echo "   ✗ Stress test failed - found failures in output"
        cat /tmp/${USERNAME}_stress_test_output.txt
        exit 1
    fi
    echo "   ✓ Stress test passed"
    echo ""
    echo "Stress test summary:"
    grep -E "===|✓|✗|SUCCESS|FAILURE|Throughput:|Total|Worker" /tmp/${USERNAME}_stress_test_output.txt | tail -30
else
    echo "   ✗ Stress test failed with exit code $?"
    cat /tmp/${USERNAME}_stress_test_output.txt
    exit 1
fi

# Verify RocksDB files were created
echo ""
echo "7. Verifying RocksDB persistence files..."
USERNAME=$(whoami)
if ls /tmp/${USERNAME}_test_rocksdb*/CURRENT > /dev/null 2>&1 || ls /tmp/${USERNAME}_rocksdb_ordered*/CURRENT > /dev/null 2>&1; then
    echo "   ✓ RocksDB database files created successfully"
    echo "   Database locations:"
    for dir in /tmp/${USERNAME}_test_rocksdb* /tmp/${USERNAME}_rocksdb_ordered* /tmp/${USERNAME}_test_stress_partitioned*; do
        if [ -d "$dir" ]; then
            echo "     - $dir ($(du -sh $dir | cut -f1))"
        fi
    done
else
    echo "   ✗ RocksDB database files not found - tests may have failed"
    exit 1
fi

echo ""
echo "=== All tests completed successfully! ==="
echo ""
echo "Tests executed:"
echo "  ✓ Basic RocksDB persistence test (test_rocksdb_persistence)"
echo "  ✓ Callback demonstration test (test_callback_demo)"
echo "  ✓ Ordered callbacks test (test_ordered_callbacks)"
echo "  ✓ Partitioned queues test (test_partitioned_queues)"
echo "  ✓ Complex stress test (test_stress_partitioned_queues)"