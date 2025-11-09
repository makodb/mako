#!/bin/bash

# Refactoring Verification Test Suite
# Tests the main refactored components for correctness and stability

set -e  # Exit on error

# Colors for output
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
NC='\033[0m' # No Color

echo "========================================"
echo "Refactoring Verification Test Suite"
echo "========================================"
echo ""

# Change to test directory
cd "$(dirname "$0")"

# Compile tests
echo "Compiling tests..."
if make clean && make; then
    echo -e "${GREEN}✓ Compilation successful${NC}"
else
    echo -e "${RED}✗ Compilation failed${NC}"
    exit 1
fi
echo ""

# Track results
PASSED=0
FAILED=0

# Function to run a test
run_test() {
    local test_name=$1
    local test_cmd=$2
    local timeout_sec=$3
    
    echo "----------------------------------------"
    echo "Running: $test_name"
    echo "----------------------------------------"
    
    if timeout $timeout_sec $test_cmd; then
        echo -e "${GREEN}✓ PASSED${NC}"
        ((PASSED++))
    else
        echo -e "${RED}✗ FAILED${NC}"
        ((FAILED++))
    fi
    echo ""
}

# Run unit tests
echo "=== Unit Tests ==="
echo ""

run_test "[1/4] PaxosAcceptor Interface Test" "./test_paxos" 5

run_test "[2/4] Reverse Loop Bug Test" "./test_loops" 5

# Run stress tests
echo "=== Stress Tests ==="
echo ""

run_test "[3/4] TransItem Memory Stress (2 minutes)" "./stress_memory" 120

run_test "[4/4] Paxos Concurrent Stress (3 minutes)" "./stress_paxos" 180

# Summary
echo "========================================"
echo "Test Summary"
echo "========================================"
echo -e "Passed: ${GREEN}$PASSED${NC}"
echo -e "Failed: ${RED}$FAILED${NC}"
echo ""

if [ $FAILED -eq 0 ]; then
    echo -e "${GREEN}All tests passed!${NC}"
    exit 0
else
    echo -e "${RED}Some tests failed!${NC}"
    exit 1
fi
