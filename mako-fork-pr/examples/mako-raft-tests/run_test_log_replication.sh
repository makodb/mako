#!/bin/bash

# =============================================================================
# Test: Preferred Replica Log Replication Test
# =============================================================================
# Purpose: Verify that 25 logs submitted to Raft are replicated to all replicas
#
# Setup:
# - 5 replicas: localhost (preferred), p1, p2, p3, p4
# - Leader submits 25 logs wrapped in TpcCommitCommand
# - All replicas track applied logs via callbacks
# - Expected: All 5 replicas apply all 25 logs
# =============================================================================

# Colors for output
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
NC='\033[0m' # No Color

echo -e "${BLUE}=================================================================${NC}"
echo -e "${BLUE}  Preferred Replica Log Replication Test${NC}"
echo -e "${BLUE}=================================================================${NC}"
echo ""

# Configuration
NAMES=("localhost" "p1" "p2" "p3" "p4")
LOG_DIR="logs/test_log_replication"
BUILD_DIR="build"
TEST_EXECUTABLE="testPreferredReplicaLogReplication"

# Create log directory
if [ -d "$LOG_DIR" ]; then
    echo "Cleaning old logs from $LOG_DIR..."
    rm -rf "$LOG_DIR"/*
    echo "✓ Old logs removed"
else
    mkdir -p "$LOG_DIR"
    echo "✓ Created log directory: $LOG_DIR"
fi
echo ""

# Check if executable exists
if [ ! -f "$BUILD_DIR/$TEST_EXECUTABLE" ]; then
    echo -e "${RED}ERROR: Executable not found: $BUILD_DIR/$TEST_EXECUTABLE${NC}"
    echo "Please run 'make' to build the test first"
    exit 1
fi

# Launch all processes in parallel
echo -e "${GREEN}Starting all 5 replicas in parallel...${NC}"
echo ""

PIDS=()
for name in "${NAMES[@]}"; do
    LOG_FILE="$LOG_DIR/${name}.log"
    echo "  [${name}] Launching... (log: $LOG_FILE)"

    ./$BUILD_DIR/$TEST_EXECUTABLE "$name" > "$LOG_FILE" 2>&1 &
    PIDS+=($!)
done

echo ""
echo -e "${YELLOW}All processes launched. PIDs: ${PIDS[@]}${NC}"
echo ""
echo "Waiting for test to complete (approximately 10-15 seconds)..."
echo ""

# Wait for all processes with progress indicator
WAIT_START=$(date +%s)
while true; do
    ALL_DONE=true
    for pid in "${PIDS[@]}"; do
        if kill -0 $pid 2>/dev/null; then
            ALL_DONE=false
            break
        fi
    done

    if [ "$ALL_DONE" = true ]; then
        break
    fi

    ELAPSED=$(($(date +%s) - WAIT_START))
    echo -ne "\r  Elapsed time: ${ELAPSED}s... "
    sleep 1
done

WAIT_END=$(date +%s)
TOTAL_TIME=$((WAIT_END - WAIT_START))
echo ""
echo -e "${GREEN}✓ All processes completed in ${TOTAL_TIME} seconds${NC}"
echo ""

# Collect results
echo "================================================================="
echo "  COLLECTING RESULTS"
echo "================================================================="
echo ""

PASS_COUNT=0
FAIL_COUNT=0

for name in "${NAMES[@]}"; do
    LOG_FILE="$LOG_DIR/${name}.log"

    echo "--- Results for $name ---"

    # Extract key metrics from log
    ROLE=$(grep "Role:" "$LOG_FILE" | tail -1 | awk '{print $NF}')
    LOGS_APPLIED=$(grep "Logs applied:" "$LOG_FILE" | tail -1 | awk '{print $3}')
    PASS_STATUS=$(grep -o "✅ PASS" "$LOG_FILE")
    FAIL_STATUS=$(grep -o "❌ FAIL" "$LOG_FILE")

    echo "  Role: $ROLE"
    echo "  Logs applied: $LOGS_APPLIED"

    if [ ! -z "$PASS_STATUS" ]; then
        echo -e "  Status: ${GREEN}PASS${NC}"
        PASS_COUNT=$((PASS_COUNT + 1))
    elif [ ! -z "$FAIL_STATUS" ]; then
        echo -e "  Status: ${RED}FAIL${NC}"
        FAIL_COUNT=$((FAIL_COUNT + 1))
    else
        echo -e "  Status: ${YELLOW}UNKNOWN${NC}"
    fi
    echo ""
done

# Overall summary
echo "================================================================="
echo "  OVERALL SUMMARY"
echo "================================================================="
echo ""
echo "  Total replicas:  5"
echo -e "  Passed:          ${GREEN}${PASS_COUNT}${NC}"
echo -e "  Failed:          ${RED}${FAIL_COUNT}${NC}"
echo ""

if [ $PASS_COUNT -eq 5 ]; then
    echo -e "${GREEN}✅ TEST PASSED: All 5 replicas successfully replicated all logs!${NC}"
    EXIT_CODE=0
elif [ $PASS_COUNT -gt 0 ]; then
    echo -e "${YELLOW}⚠️  TEST PARTIAL: Only $PASS_COUNT/5 replicas passed${NC}"
    EXIT_CODE=1
else
    echo -e "${RED}❌ TEST FAILED: No replicas successfully replicated all logs${NC}"
    EXIT_CODE=1
fi

echo ""
echo "================================================================="
echo "  Log files available in: $LOG_DIR/"
echo "================================================================="
echo ""

exit $EXIT_CODE
