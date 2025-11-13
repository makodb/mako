#!/bin/bash

# Mako Unit Test Suite
# Tests individual components for correctness

# Colors for output
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
NC='\033[0m' # No Color

echo "========================================"
echo "Mako Unit Test Suite"
echo "========================================"
echo ""

# Change to test directory
cd "$(dirname "$0")"

# Parse command line arguments
RUN_MODE="all"
SPECIFIC_TEST=""

if [ $# -gt 0 ]; then
    case "$1" in
        --list)
            echo "Available unit tests:"
            echo ""
            echo "STO Tests:"
            echo "  1. test_hashtable"
            echo "  2. test_vector"
            echo "  3. test_queue"
            echo "  4. test_priority_queue"
            echo "  5. test_rbtree"
            echo "  6. test_transaction_helpers"
            echo "  7. test_transitem"
            echo ""
            echo "Silo Tests:"
            echo "  8. test_txn_core"
            echo "  9. test_mbta_wrapper"
            echo "  10. test_utilities"
            echo ""
            echo "Usage:"
            echo "  ./run_unit_tests.sh              # Run all tests"
            echo "  ./run_unit_tests.sh --list       # List available tests"
            echo "  ./run_unit_tests.sh <test_name>  # Run specific test"
            echo ""
            exit 0
            ;;
        *)
            RUN_MODE="single"
            SPECIFIC_TEST="$1"
            ;;
    esac
fi

# Compile tests
echo "Compiling unit tests..."
if make clean > /dev/null 2>&1 && make unit; then
    echo -e "${GREEN}✓ Compilation successful${NC}"
else
    echo -e "${RED}✗ Compilation failed${NC}"
    exit 1
fi
echo ""

# Track results
PASSED=0
FAILED=0
FAILED_TESTS=()

# Function to run a test
run_test() {
    local test_name=$1
    local test_path=$2
    local timeout_sec=$3
    
    # Skip if running specific test and this isn't it
    if [ "$RUN_MODE" = "single" ]; then
        if [ "$test_name" != "$SPECIFIC_TEST" ]; then
            return 0
        fi
    fi
    
    echo "----------------------------------------"
    echo -e "${BLUE}Running: $test_name${NC}"
    echo "----------------------------------------"
    
    if timeout $timeout_sec $test_path; then
        echo -e "${GREEN}✓ PASSED${NC}"
        ((PASSED++))
    else
        local exit_code=$?
        echo -e "${RED}✗ FAILED (exit code: $exit_code)${NC}"
        ((FAILED++))
        FAILED_TESTS+=("$test_name")
    fi
    echo ""
}

# Run STO unit tests
echo "=== STO Data Structure Tests ==="
echo ""

run_test "test_hashtable" "./unit/sto/test_hashtable" 90
run_test "test_vector" "./unit/sto/test_vector" 90
run_test "test_queue" "./unit/sto/test_queue" 90
run_test "test_priority_queue" "./unit/sto/test_priority_queue" 90
run_test "test_rbtree" "./unit/sto/test_rbtree" 90
run_test "test_transaction_helpers" "./unit/sto/test_transaction_helpers" 90
run_test "test_transitem" "./unit/sto/test_transitem" 90

# Run Silo unit tests
echo "=== Silo Transaction Tests ==="
echo ""

run_test "test_txn_core" "./unit/silo/test_txn_core" 90
run_test "test_mbta_wrapper" "./unit/silo/test_mbta_wrapper" 90
run_test "test_utilities" "./unit/silo/test_utilities" 90

# Summary
echo "========================================"
echo "Test Summary"
echo "========================================"
echo -e "Passed: ${GREEN}$PASSED${NC}"
echo -e "Failed: ${RED}$FAILED${NC}"
echo ""

if [ $FAILED -eq 0 ]; then
    echo -e "${GREEN}✓ All tests passed!${NC}"
    exit 0
else
    echo -e "${RED}✗ Failed tests:${NC}"
    for test in "${FAILED_TESTS[@]}"; do
        echo -e "  ${RED}- $test${NC}"
    done
    echo ""
    exit 1
fi
