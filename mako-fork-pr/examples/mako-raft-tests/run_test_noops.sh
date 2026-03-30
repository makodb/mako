#!/bin/bash

# run_test_noops.sh
#
# Shell script to run NO-OPS watermark synchronization test with preferred leader
# Tests:
# - NO-OPS propagation across all replicas
# - Epoch tracking and ordering
# - Preferred leader election
# - Regular log replication after NO-OPS watermark sync

set -e  # Exit on error

# Colors for output
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
NC='\033[0m' # No Color

# Test configuration
TEST_NAME="NO-OPS Watermark Synchronization Test"
TEST_DURATION=25  # seconds (startup + NO-OPS + logs + verification)
PROCESSES=("localhost" "p1" "p2" "p3" "p4")

# Get absolute paths
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"
BUILD_DIR="${PROJECT_ROOT}/build"
TEST_EXEC="${BUILD_DIR}/testNoOps"
LOG_DIR="${SCRIPT_DIR}/logs_noops_test"

echo -e "${BLUE}=================================================================${NC}"
echo -e "${BLUE}${TEST_NAME}${NC}"
echo -e "${BLUE}=================================================================${NC}"
echo ""

# Create log directory
mkdir -p "${LOG_DIR}"
rm -f "${LOG_DIR}"/*.log

# Clean up any previous instances
echo -e "${YELLOW}Cleaning up any previous test instances...${NC}"
pkill -9 -f testNoOps || true
sleep 1

# Check if test executable exists
if [ ! -f "${TEST_EXEC}" ]; then
    echo -e "${RED}ERROR: Test executable not found: ${TEST_EXEC}${NC}"
    echo -e "${YELLOW}Building test executable...${NC}"

    cd "${PROJECT_ROOT}"
    if [ ! -d "build" ]; then
        echo -e "${YELLOW}Creating build directory...${NC}"
        cmake -B build -DMAKO_USE_RAFT=ON
    fi

    cmake --build build --target testNoOps -j$(nproc)
    cd "${SCRIPT_DIR}"

    if [ ! -f "${TEST_EXEC}" ]; then
        echo -e "${RED}ERROR: Build failed!${NC}"
        exit 1
    fi
    echo -e "${GREEN}✓ Build successful${NC}"
fi

echo -e "${GREEN}✓ Test executable found${NC}"
echo ""

# Launch all processes in parallel
echo -e "${YELLOW}Launching 5 Raft replicas...${NC}"
echo ""

PIDS=()
for proc in "${PROCESSES[@]}"; do
    LOG_FILE="${LOG_DIR}/${proc}.log"

    if [ "$proc" == "localhost" ]; then
        echo -e "${GREEN}Starting ${proc} (PREFERRED LEADER)...${NC}"
    else
        echo -e "Starting ${proc}..."
    fi

    ${TEST_EXEC} ${proc} > "${LOG_FILE}" 2>&1 &
    PIDS+=($!)

    echo "  PID: ${PIDS[-1]}, Log: ${LOG_FILE}"
done

echo ""
echo -e "${GREEN}✓ All processes launched${NC}"
echo ""

# Display PIDs
echo "Process PIDs:"
for i in "${!PROCESSES[@]}"; do
    if [ "${PROCESSES[$i]}" == "localhost" ]; then
        echo -e "  ${GREEN}${PROCESSES[$i]} (PREFERRED): ${PIDS[$i]}${NC}"
    else
        echo "  ${PROCESSES[$i]}: ${PIDS[$i]}"
    fi
done
echo ""

# Monitor test execution
echo -e "${YELLOW}Test running for approximately ${TEST_DURATION} seconds...${NC}"
echo -e "${YELLOW}Monitoring logs for progress...${NC}"
echo ""

# Wait for test to complete or timeout
ELAPSED=0
CHECK_INTERVAL=2
ALL_EXITED=false

while [ $ELAPSED -lt $TEST_DURATION ]; do
    sleep $CHECK_INTERVAL
    ELAPSED=$((ELAPSED + CHECK_INTERVAL))

    # Check if all processes have exited
    RUNNING_COUNT=0
    for pid in "${PIDS[@]}"; do
        if kill -0 $pid 2>/dev/null; then
            RUNNING_COUNT=$((RUNNING_COUNT + 1))
        fi
    done

    if [ $RUNNING_COUNT -eq 0 ]; then
        echo -e "${GREEN}All processes completed at ${ELAPSED}s${NC}"
        ALL_EXITED=true
        break
    fi

    echo "[${ELAPSED}s] ${RUNNING_COUNT}/5 processes still running..."

    # Show latest status from each log
    if [ $((ELAPSED % 10)) -eq 0 ]; then
        echo ""
        echo "=== Status Update at ${ELAPSED}s ==="
        for proc in "${PROCESSES[@]}"; do
            LOG_FILE="${LOG_DIR}/${proc}.log"
            if [ -f "${LOG_FILE}" ]; then
                # Extract latest meaningful line
                LATEST=$(tail -5 "${LOG_FILE}" | grep -E "(NO-OPS|Regular log|LEADER|FOLLOWER)" | tail -1 || echo "No status")
                echo "  ${proc}: ${LATEST}"
            fi
        done
        echo ""
    fi
done

echo ""

# If processes didn't exit naturally, kill them
if [ "$ALL_EXITED" = false ]; then
    echo -e "${YELLOW}Timeout reached, terminating processes...${NC}"
    for pid in "${PIDS[@]}"; do
        kill -9 $pid 2>/dev/null || true
    done
    sleep 1
fi

echo -e "${BLUE}=================================================================${NC}"
echo -e "${BLUE}TEST RESULTS${NC}"
echo -e "${BLUE}=================================================================${NC}"
echo ""

# Collect results from each process
PASS_COUNT=0
FAIL_COUNT=0

for proc in "${PROCESSES[@]}"; do
    LOG_FILE="${LOG_DIR}/${proc}.log"

    if [ "$proc" == "localhost" ]; then
        echo -e "${GREEN}=== ${proc} (PREFERRED LEADER) ===${NC}"
    else
        echo "=== ${proc} ==="
    fi

    if [ ! -f "${LOG_FILE}" ]; then
        echo -e "  ${RED}✗ No log file found${NC}"
        FAIL_COUNT=$((FAIL_COUNT + 1))
        echo ""
        continue
    fi

    # Extract key metrics
    NOOPS_APPLIED=$(grep "NO-OPS applied:" "${LOG_FILE}" | tail -1 | grep -oP '\d+/\d+' || echo "?/?")
    REGULAR_APPLIED=$(grep "Regular logs applied:" "${LOG_FILE}" | tail -1 | grep -oP '\d+/\d+' || echo "?/?")
    MAX_EPOCH=$(grep "Max epoch seen:" "${LOG_FILE}" | tail -1 | awk '{print $NF}' || echo "?")
    ROLE=$(grep "Role:" "${LOG_FILE}" | tail -1 | awk '{print $2, $3}' || echo "?")

    echo "  Role: ${ROLE}"
    echo "  NO-OPS applied: ${NOOPS_APPLIED}"
    echo "  Max epoch seen: ${MAX_EPOCH}"
    echo "  Regular logs applied: ${REGULAR_APPLIED}"

    # Check for overall pass/fail
    if grep -q "OVERALL: ALL TESTS PASSED" "${LOG_FILE}"; then
        echo -e "  ${GREEN}✅ PASS${NC}"
        PASS_COUNT=$((PASS_COUNT + 1))
    elif grep -q "OVERALL: SOME TESTS FAILED" "${LOG_FILE}"; then
        echo -e "  ${RED}❌ FAIL${NC}"
        FAIL_COUNT=$((FAIL_COUNT + 1))

        # Show why it failed
        echo "  Failures:"
        grep "❌ FAIL:" "${LOG_FILE}" | sed 's/^/    /'
    else
        echo -e "  ${RED}✗ Test did not complete${NC}"
        FAIL_COUNT=$((FAIL_COUNT + 1))
    fi

    echo ""
done

# Overall test result
echo -e "${BLUE}=================================================================${NC}"
if [ $PASS_COUNT -eq 5 ] && [ $FAIL_COUNT -eq 0 ]; then
    echo -e "${GREEN}✅✅✅ OVERALL TEST: PASS (5/5 replicas passed) ✅✅✅${NC}"
    EXIT_CODE=0
else
    echo -e "${RED}❌❌❌ OVERALL TEST: FAIL (${PASS_COUNT}/5 passed, ${FAIL_COUNT}/5 failed) ❌❌❌${NC}"
    EXIT_CODE=1
fi
echo -e "${BLUE}=================================================================${NC}"
echo ""

# Show summary of NO-OPS propagation
echo "=== NO-OPS Propagation Summary ==="
for proc in "${PROCESSES[@]}"; do
    LOG_FILE="${LOG_DIR}/${proc}.log"
    if [ -f "${LOG_FILE}" ]; then
        echo "${proc}:"
        grep "🔔 NO-OPS epoch=" "${LOG_FILE}" | sed 's/^/  /' || echo "  No NO-OPS received"
    fi
done
echo ""

# Provide log file locations
echo "Full logs available at:"
for proc in "${PROCESSES[@]}"; do
    echo "  ${LOG_DIR}/${proc}.log"
done
echo ""

# Cleanup
echo -e "${YELLOW}Cleaning up...${NC}"
pkill -9 -f testNoOps || true
sleep 1
echo -e "${GREEN}✓ Cleanup complete${NC}"
echo ""

exit $EXIT_CODE
