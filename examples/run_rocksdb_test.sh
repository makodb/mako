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

echo "1. Building project..."
make -j32

# Clean test directories
echo "2. Cleaning test RocksDB directories..."
rm -rf /tmp/test_rocksdb* /tmp/mako_rocksdb* /tmp/rocksdb_ordered*

# Run test
echo "3. Running RocksDB persistence tests..."
if ./build/test_rocksdb_persistence > /tmp/rocksdb_test_output.txt 2>&1; then
    echo "   ✓ Basic persistence tests passed"
    echo ""
    echo "Test output summary:"
    grep "===" /tmp/rocksdb_test_output.txt
    echo ""
    # Show performance metrics
    grep -E "Throughput:|Time taken:" /tmp/rocksdb_test_output.txt | head -3
else
    echo "   ✗ Basic persistence tests failed"
    cat /tmp/rocksdb_test_output.txt
    exit 1
fi

# Run callback demo test
echo ""
echo "4. Running callback demonstration test..."
if ./build/test_callback_demo > /tmp/callback_demo_output.txt 2>&1; then
    echo "   ✓ Callback demo passed"
    echo ""
    echo "Callback demo output:"
    grep -E "===|Total|Persisted:|Failed:" /tmp/callback_demo_output.txt
else
    echo "   ✗ Callback demo failed"
    cat /tmp/callback_demo_output.txt
    exit 1
fi

# Run ordered callbacks test
echo ""
echo "5. Running ordered callbacks test..."
if ./build/test_ordered_callbacks > /tmp/ordered_callbacks_output.txt 2>&1; then
    echo "   ✓ Ordered callbacks test passed"
    echo ""
    echo "Ordered callbacks output:"
    grep -E "===|✓|✗|ERROR" /tmp/ordered_callbacks_output.txt
else
    echo "   ✗ Ordered callbacks test failed"
    cat /tmp/ordered_callbacks_output.txt
    exit 1
fi

# Run partitioned queues test
echo ""
echo "6. Running partitioned queues test..."
if ./build/test_partitioned_queues > /tmp/partitioned_queues_output.txt 2>&1; then
    echo "   ✓ Partitioned queues test passed"
    echo ""
    echo "Partitioned queues output:"
    grep -E "===|✓|✗|Throughput:|Worker" /tmp/partitioned_queues_output.txt
else
    echo "   ✗ Partitioned queues test failed"
    cat /tmp/partitioned_queues_output.txt
    exit 1
fi

# Run stress test
echo ""
echo "7. Running complex stress test (20 threads, 10 partitions, mixed load)..."
if ./build/test_stress_partitioned_queues > /tmp/stress_test_output.txt 2>&1; then
    # Check if test actually passed by looking for FAILURE in output
    if grep -q "FAILURE" /tmp/stress_test_output.txt; then
        echo "   ✗ Stress test failed - found failures in output"
        cat /tmp/stress_test_output.txt
        exit 1
    fi
    echo "   ✓ Stress test passed"
    echo ""
    echo "Stress test summary:"
    grep -E "===|✓|✗|SUCCESS|FAILURE|Throughput:|Total|Worker" /tmp/stress_test_output.txt | tail -30
else
    echo "   ✗ Stress test failed with exit code $?"
    cat /tmp/stress_test_output.txt
    exit 1
fi

# Verify RocksDB files were created
echo ""
echo "8. Verifying RocksDB persistence files..."
if ls /tmp/test_rocksdb*/CURRENT > /dev/null 2>&1 || ls /tmp/rocksdb_ordered*/CURRENT > /dev/null 2>&1; then
    echo "   ✓ RocksDB database files created successfully"
    echo "   Database locations:"
    for dir in /tmp/test_rocksdb* /tmp/rocksdb_ordered* /tmp/test_stress_partitioned*; do
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