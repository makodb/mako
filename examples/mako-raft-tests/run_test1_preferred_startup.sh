#!/bin/bash

###############################################################################
# Test 1: Preferred Replica Startup Test (with TimeoutNow Protocol)
###############################################################################
# Purpose: Verify that the preferred replica (localhost) becomes leader
#          via TimeoutNow leadership transfer protocol
#
# Test Scenario:
# - Start 5-node Raft cluster (localhost, p1, p2, p3, p4)
# - localhost is configured as preferred leader
# - Any replica wins initial election (standard Raft)
# - Non-preferred leader waits 5s for cluster to stabilize
# - Non-preferred leader sends TimeoutNow to localhost
# - localhost receives TimeoutNow and starts election immediately
# - localhost wins election and becomes leader
# - localhost remains leader for 30 seconds
#
# Expected Result:
# - localhost becomes leader within 7 seconds (5s wait + 2s transfer)
# - localhost remains stable leader for full 30 seconds
# - Clean leadership transfer via TimeoutNow RPC
# - Test exits with code 0 (success)
###############################################################################

set -e  # Exit on error

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"
BUILD_DIR="$PROJECT_ROOT/build"
TEST_BINARY="$BUILD_DIR/testPreferredReplicaStartup"

# Colors for output
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
NC='\033[0m' # No Color

echo -e "${BLUE}===============================================================================${NC}"
echo -e "${BLUE}Test 1: Preferred Replica Startup Test${NC}"
echo -e "${BLUE}===============================================================================${NC}"
echo ""

# Check if build is complete
if [ ! -f "$TEST_BINARY" ]; then
    echo -e "${YELLOW}Test binary not found. Building...${NC}"
    cd "$PROJECT_ROOT"
    make -j8
    if [ $? -ne 0 ]; then
        echo -e "${RED}Build failed!${NC}"
        exit 1
    fi
    echo -e "${GREEN}Build complete${NC}"
    echo ""
fi

# Verify test binary exists
if [ ! -f "$TEST_BINARY" ]; then
    echo -e "${RED}ERROR: Test binary not found at: $TEST_BINARY${NC}"
    echo -e "${RED}Please build the project first: make -j8${NC}"
    exit 1
fi

echo -e "${GREEN}✓ Test binary found${NC}"
echo ""

# Create log directory and clean old logs
LOG_DIR="$SCRIPT_DIR/logs/test1_startup"
mkdir -p "$LOG_DIR"

# Remove old logs
echo -e "${YELLOW}Cleaning old logs from $LOG_DIR...${NC}"
rm -f "$LOG_DIR"/*.log 2>/dev/null || true
echo -e "${GREEN}✓ Old logs removed${NC}"
echo ""

# Use simple log file names (no timestamp)
LOG_FILE="$LOG_DIR/test1"

echo -e "${BLUE}Test Configuration:${NC}"
echo "  Binary:    $TEST_BINARY"
echo "  Log dir:   $LOG_DIR"
echo "  Log files: localhost.log, p1.log, p2.log, p3.log, p4.log"
echo ""

# Cleanup function
cleanup() {
    echo ""
    echo -e "${YELLOW}Cleaning up processes...${NC}"
    pkill -9 -f testPreferredReplicaStartup || true
    sleep 2
    echo -e "${GREEN}Cleanup complete${NC}"
}

trap cleanup EXIT

# Kill any existing test processes (more thorough)
echo -e "${YELLOW}Cleaning up any existing test processes...${NC}"
pkill -9 -f testPreferredReplicaStartup || true
pkill -9 -f testLeaderStability || true
pkill -9 -f testBasicSetup || true
pkill -9 -f testElectionStability || true
sleep 2
echo -e "${GREEN}✓ Process cleanup complete${NC}"
echo ""

echo -e "${BLUE}===============================================================================${NC}"
echo -e "${BLUE}Starting Test Processes${NC}"
echo -e "${BLUE}===============================================================================${NC}"
echo ""


# Start localhost (preferred leader)
echo -e "${BLUE}[1/5] Starting localhost (preferred leader)...${NC}"
"$TEST_BINARY" localhost > "${LOG_DIR}/localhost.log" 2>&1 &
LOCALHOST_PID=$!
echo -e "${GREEN}  ✓ localhost started (PID: $LOCALHOST_PID)${NC}"

# Start p1
echo -e "${BLUE}[2/5] Starting p1 (follower)...${NC}"
"$TEST_BINARY" p1 > "${LOG_DIR}/p1.log" 2>&1 &
P1_PID=$!
echo -e "${GREEN}  ✓ p1 started (PID: $P1_PID)${NC}"

# Start p2
echo -e "${BLUE}[3/5] Starting p2 (follower)...${NC}"
"$TEST_BINARY" p2 > "${LOG_DIR}/p2.log" 2>&1 &
P2_PID=$!
echo -e "${GREEN}  ✓ p2 started (PID: $P2_PID)${NC}"

# Start p3
echo -e "${BLUE}[4/5] Starting p3 (follower)...${NC}"
"$TEST_BINARY" p3 > "${LOG_DIR}/p3.log" 2>&1 &
P3_PID=$!
echo -e "${GREEN}  ✓ p3 started (PID: $P3_PID)${NC}"

# Start p4
echo -e "${BLUE}[5/5] Starting p4 (follower)...${NC}"
"$TEST_BINARY" p4 > "${LOG_DIR}/p4.log" 2>&1 &
P4_PID=$!
echo -e "${GREEN}  ✓ p4 started (PID: $P4_PID)${NC}"



echo ""
echo -e "${GREEN}All processes started successfully${NC}"
echo -e "${BLUE}Process IDs:${NC}"
echo "  localhost: $LOCALHOST_PID"
echo "  p1:        $P1_PID"
echo "  p2:        $P2_PID"
echo "  p3:        $P3_PID"
echo "  p4:        $P4_PID"
echo ""

# Monitor test progress
echo -e "${BLUE}===============================================================================${NC}"
echo -e "${BLUE}Test Running (duration: ~35 seconds)${NC}"
echo -e "${BLUE}===============================================================================${NC}"
echo ""
echo -e "${YELLOW}Monitoring test progress...${NC}"
echo ""

# Wait for tests to complete (30s test + 5s buffer for startup and shutdown)
TEST_DURATION=35

for i in $(seq 1 $TEST_DURATION); do
    echo -ne "\r${BLUE}[Progress]${NC} $i / $TEST_DURATION seconds elapsed..."
    sleep 1

    # Check if any process has exited early
    if ! kill -0 $LOCALHOST_PID 2>/dev/null; then
        echo ""
        echo -e "${YELLOW}localhost process exited early${NC}"
        break
    fi
    if ! kill -0 $P1_PID 2>/dev/null; then
        echo ""
        echo -e "${YELLOW}p1 process exited early${NC}"
        break
    fi
    if ! kill -0 $P2_PID 2>/dev/null; then
        echo ""
        echo -e "${YELLOW}p2 process exited early${NC}"
        break
    fi
    if ! kill -0 $P3_PID 2>/dev/null; then
        echo ""
        echo -e "${YELLOW}p3 process exited early${NC}"
        break
    fi
    if ! kill -0 $P4_PID 2>/dev/null; then
        echo ""
        echo -e "${YELLOW}p4 process exited early${NC}"
        break
    fi
done

echo ""
echo ""

# Wait for processes to finish
echo -e "${BLUE}Waiting for test processes to complete...${NC}"

wait $LOCALHOST_PID 2>/dev/null
LOCALHOST_EXIT=$?

wait $P1_PID 2>/dev/null
P1_EXIT=$?

wait $P2_PID 2>/dev/null
P2_EXIT=$?

wait $P3_PID 2>/dev/null
P3_EXIT=$?

wait $P4_PID 2>/dev/null
P4_EXIT=$?

echo -e "${GREEN}All processes completed${NC}"
echo ""

# Display results
echo -e "${BLUE}===============================================================================${NC}"
echo -e "${BLUE}Test Results${NC}"
echo -e "${BLUE}===============================================================================${NC}"
echo ""

echo -e "${BLUE}Exit Codes:${NC}"
echo "  localhost: $LOCALHOST_EXIT"
echo "  p1:        $P1_EXIT"
echo "  p2:        $P2_EXIT"
echo "  p3:        $P3_EXIT"
echo "  p4:        $P4_EXIT"
echo ""

# Analyze logs
echo -e "${BLUE}Analyzing Results...${NC}"
echo ""

# Check localhost became leader
LOCALHOST_LEADER=$(grep "BECAME LEADER" "${LOG_DIR}/localhost.log" | wc -l)
P1_LEADER=$(grep "BECAME LEADER" "${LOG_DIR}/p1.log" | wc -l)
P2_LEADER=$(grep "BECAME LEADER" "${LOG_DIR}/p2.log" | wc -l)
P3_LEADER=$(grep "BECAME LEADER" "${LOG_DIR}/p3.log" | wc -l)
P4_LEADER=$(grep "BECAME LEADER" "${LOG_DIR}/p4.log" | wc -l)

echo -e "${BLUE}Leadership Events:${NC}"
echo "  localhost became leader: $LOCALHOST_LEADER time(s)"
echo "  p1 became leader:        $P1_LEADER time(s)"
echo "  p2 became leader:        $P2_LEADER time(s)"
echo "  p3 became leader:        $P3_LEADER time(s)"
echo "  p4 became leader:        $P4_LEADER time(s)"
echo ""

# Check test verdicts
LOCALHOST_PASSED=$(grep "TEST PASSED" "${LOG_DIR}/localhost.log" | wc -l)
P1_PASSED=$(grep "TEST PASSED" "${LOG_DIR}/p1.log" | wc -l)
P2_PASSED=$(grep "TEST PASSED" "${LOG_DIR}/p2.log" | wc -l)
P3_PASSED=$(grep "TEST PASSED" "${LOG_DIR}/p3.log" | wc -l)
P4_PASSED=$(grep "TEST PASSED" "${LOG_DIR}/p4.log" | wc -l)

echo -e "${BLUE}Test Verdicts:${NC}"
if [ $LOCALHOST_PASSED -eq 1 ]; then
    echo -e "  localhost: ${GREEN}✅ PASSED${NC}"
else
    echo -e "  localhost: ${RED}❌ FAILED${NC}"
fi

if [ $P1_PASSED -eq 1 ]; then
    echo -e "  p1:        ${GREEN}✅ PASSED${NC}"
else
    echo -e "  p1:        ${RED}❌ FAILED${NC}"
fi

if [ $P2_PASSED -eq 1 ]; then
    echo -e "  p2:        ${GREEN}✅ PASSED${NC}"
else
    echo -e "  p2:        ${RED}❌ FAILED${NC}"
fi

if [ $P3_PASSED -eq 1 ]; then
    echo -e "  p3:        ${GREEN}✅ PASSED${NC}"
else
    echo -e "  p3:        ${RED}❌ FAILED${NC}"
fi

if [ $P4_PASSED -eq 1 ]; then
    echo -e "  p4:        ${GREEN}✅ PASSED${NC}"
else
    echo -e "  p4:        ${RED}❌ FAILED${NC}"
fi
echo ""

# Overall result
OVERALL_PASS=0
if [ $LOCALHOST_EXIT -eq 0 ] && [ $P1_EXIT -eq 0 ] && [ $P2_EXIT -eq 0 ] && \
   [ $P3_EXIT -eq 0 ] && [ $P4_EXIT -eq 0 ] && \
   [ $LOCALHOST_LEADER -ge 1 ] && [ $P1_LEADER -eq 0 ] && [ $P2_LEADER -eq 0 ] && \
   [ $P3_LEADER -eq 0 ] && [ $P4_LEADER -eq 0 ]; then
    OVERALL_PASS=1
fi

echo -e "${BLUE}===============================================================================${NC}"
if [ $OVERALL_PASS -eq 1 ]; then
    echo -e "${GREEN}🎉 TEST 1: OVERALL SUCCESS${NC}"
    echo ""
    echo -e "${GREEN}✓ localhost became leader (preferred replica system working)${NC}"
    echo -e "${GREEN}✓ p1, p2, p3, and p4 remained followers${NC}"
    echo -e "${GREEN}✓ All processes reported success${NC}"
else
    echo -e "${RED}❌ TEST 1: OVERALL FAILURE${NC}"
    echo ""
    if [ $LOCALHOST_LEADER -eq 0 ]; then
        echo -e "${RED}✗ localhost did NOT become leader${NC}"
    fi
    if [ $P1_LEADER -gt 0 ]; then
        echo -e "${RED}✗ p1 became leader (unexpected)${NC}"
    fi
    if [ $P2_LEADER -gt 0 ]; then
        echo -e "${RED}✗ p2 became leader (unexpected)${NC}"
    fi
    if [ $P3_LEADER -gt 0 ]; then
        echo -e "${RED}✗ p3 became leader (unexpected)${NC}"
    fi
    if [ $P4_LEADER -gt 0 ]; then
        echo -e "${RED}✗ p4 became leader (unexpected)${NC}"
    fi
    if [ $LOCALHOST_EXIT -ne 0 ] || [ $P1_EXIT -ne 0 ] || [ $P2_EXIT -ne 0 ] || \
       [ $P3_EXIT -ne 0 ] || [ $P4_EXIT -ne 0 ]; then
        echo -e "${RED}✗ Some processes exited with errors${NC}"
    fi
fi
echo -e "${BLUE}===============================================================================${NC}"
echo ""

echo -e "${BLUE}Log files saved to:${NC}"
echo "  ${LOG_DIR}/localhost.log"
echo "  ${LOG_DIR}/p1.log"
echo "  ${LOG_DIR}/p2.log"
echo "  ${LOG_DIR}/p3.log"
echo "  ${LOG_DIR}/p4.log"
echo ""

if [ $OVERALL_PASS -ne 1 ]; then
    echo -e "${YELLOW}To view logs:${NC}"
    echo "  tail -100 ${LOG_DIR}/localhost.log"
    echo "  tail -100 ${LOG_DIR}/p1.log"
    echo "  tail -100 ${LOG_DIR}/p2.log"
    echo "  tail -100 ${LOG_DIR}/p3.log"
    echo "  tail -100 ${LOG_DIR}/p4.log"
    echo ""
fi

# Return appropriate exit code
if [ $OVERALL_PASS -eq 1 ]; then
    exit 0
else
    exit 1
fi
