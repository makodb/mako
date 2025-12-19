#!/bin/bash

# Script to reproduce and debug eRPC crash with gdb
# Runs multiple iterations, starting processes under gdb to catch crashes
# It will run to the end, and keep all gdb crash information

set -e

# Configuration
MAX_ITERATIONS=50
WAIT_TIME=60  # seconds to wait for crash
NSHARD=2
THREADS=6
CLUSTER="localhost"

# Directories
LOG_DIR="./crash_logs"
mkdir -p "$LOG_DIR"

# Color output
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
NC='\033[0m' # No Color

echo "========================================"
echo "eRPC Crash Reproduction Script"
echo "========================================"
echo "Max iterations: $MAX_ITERATIONS"
echo "Wait time per iteration: ${WAIT_TIME}s"
echo "Log directory: $LOG_DIR"
echo "========================================"
echo ""

cleanup() {
    echo -e "${YELLOW}Cleaning up processes...${NC}"
    bash kill.sh 2>/dev/null || true
    sleep 1
    rm -f nfs_sync_127.0.0.1_6001_load_phase_* 2>/dev/null || true
    rm -f /tmp/gdb_shard*.log 2>/dev/null || true
}

# Initial cleanup
cleanup

# Main loop
for i in $(seq 1 $MAX_ITERATIONS); do
    echo ""
    echo "========================================"
    echo -e "${GREEN}Iteration $i / $MAX_ITERATIONS${NC}"
    echo "========================================"

    # Cleanup from previous iteration
    cleanup
    sleep 2

    # Prepare gdb command files for each shard
    for shard in 0 1; do
        cat > /tmp/gdb_commands_shard${shard}.txt <<EOF
set pagination off
set logging file ${LOG_DIR}/gdb_shard${shard}_iter${i}.log
set logging overwrite on
set logging on
run
echo \n========== CRASH DETECTED ==========\n
echo Shard ${shard} crashed!\n
echo ====================================\n
thread apply all bt full
quit
EOF
    done

    # Build commands for each shard
    CMD_BASE="./build/dbtest --num-threads $THREADS --shard-config ./src/mako/config/local-shards${NSHARD}-warehouses${THREADS}.yml -F config/occ_paxos.yml -P $CLUSTER"

    CMD_SHARD0="$CMD_BASE --shard-index 0 -F config/1leader_2followers/paxos${THREADS}_shardidx0.yml"
    CMD_SHARD1="$CMD_BASE --shard-index 1 -F config/1leader_2followers/paxos${THREADS}_shardidx1.yml"

    echo "Starting shard 0 under gdb..."
    MAKO_TRANSPORT=erpc gdb -batch -x /tmp/gdb_commands_shard0.txt --args $CMD_SHARD0 > ${LOG_DIR}/gdb_output_shard0_iter${i}.log 2>&1 &
    PID_SHARD0=$!

    echo "Starting shard 1 under gdb..."
    MAKO_TRANSPORT=erpc gdb -batch -x /tmp/gdb_commands_shard1.txt --args $CMD_SHARD1 > ${LOG_DIR}/gdb_output_shard1_iter${i}.log 2>&1 &
    PID_SHARD1=$!

    echo "Shard 0 PID: $PID_SHARD0"
    echo "Shard 1 PID: $PID_SHARD1"
    echo "Waiting ${WAIT_TIME} seconds for crash..."

    # Monitor processes
    CRASH_DETECTED=0
    for ((t=0; t<$WAIT_TIME; t++)); do
        sleep 1

        # Check if either process has exited
        if ! kill -0 $PID_SHARD0 2>/dev/null; then
            # Process exited, check if it was a crash or normal exit
            wait $PID_SHARD0
            EXIT_CODE=$?
            if [ $EXIT_CODE -ne 0 ]; then
                echo -e "${RED}*** CRASH DETECTED in shard 0 at ${t}s! Exit code: $EXIT_CODE ***${NC}"
                CRASH_DETECTED=1
                CRASHED_SHARD=0
            else
                echo -e "${GREEN}Shard 0 exited normally at ${t}s${NC}"
            fi
            break
        fi

        if ! kill -0 $PID_SHARD1 2>/dev/null; then
            # Process exited, check if it was a crash or normal exit
            wait $PID_SHARD1
            EXIT_CODE=$?
            if [ $EXIT_CODE -ne 0 ]; then
                echo -e "${RED}*** CRASH DETECTED in shard 1 at ${t}s! Exit code: $EXIT_CODE ***${NC}"
                CRASH_DETECTED=1
                CRASHED_SHARD=1
            else
                echo -e "${GREEN}Shard 1 exited normally at ${t}s${NC}"
            fi
            break
        fi

        # Progress indicator
        if [ $((t % 10)) -eq 0 ] && [ $t -gt 0 ]; then
            echo "  ... still running (${t}s elapsed)"
        fi
    done

    # Wait for the other process to finish if one already exited
    wait $PID_SHARD0 2>/dev/null || true
    wait $PID_SHARD1 2>/dev/null || true

    # Check results
    if [ $CRASH_DETECTED -eq 1 ]; then
        echo ""
        echo "========================================"
        echo -e "${RED}CRASH FOUND ON ITERATION $i!${NC}"
        echo "========================================"

        # Wait a moment for gdb to finish writing
        sleep 2

        # Kill remaining processes
        cleanup

        # Display the program output (contains error messages)
        echo ""
        echo "========== PROGRAM OUTPUT (CRASHED SHARD) =========="
        tail -150 ${LOG_DIR}/gdb_output_shard${CRASHED_SHARD}_iter${i}.log 2>/dev/null || echo "Output file not found"
        echo "===================================================="
        echo ""

        # Display gdb backtrace if available (only works for signal crashes)
        if grep -q "CRASH DETECTED" ${LOG_DIR}/gdb_shard${CRASHED_SHARD}_iter${i}.log 2>/dev/null; then
            echo "========== GDB BACKTRACE =========="
            cat ${LOG_DIR}/gdb_shard${CRASHED_SHARD}_iter${i}.log
            echo "==================================="
            echo ""
        fi

        # Also check the other shard's output
        echo "========== OTHER SHARD OUTPUT =========="
        OTHER_SHARD=$((1 - CRASHED_SHARD))
        tail -100 ${LOG_DIR}/gdb_output_shard${OTHER_SHARD}_iter${i}.log 2>/dev/null || echo "Other shard output not available"
        echo "========================================"
        echo ""

        echo -e "${GREEN}Crash logs saved to:${NC}"
        echo "  - ${LOG_DIR}/gdb_output_shard${CRASHED_SHARD}_iter${i}.log (program output - crashed)"
        echo "  - ${LOG_DIR}/gdb_shard${CRASHED_SHARD}_iter${i}.log (gdb backtrace - if crash was a signal)"
        echo "  - ${LOG_DIR}/gdb_output_shard${OTHER_SHARD}_iter${i}.log (other shard output)"
        echo ""
        echo -e "${YELLOW}Stopping script - please analyze the crash!${NC}"
        echo "To view full output:"
        echo "  cat ${LOG_DIR}/gdb_output_shard${CRASHED_SHARD}_iter${i}.log"

        exit 0
    else
        echo -e "${GREEN}Iteration $i completed without crash${NC}"
        # Cleanup processes that are still running
        cleanup
    fi
done

echo ""
echo "========================================"
echo -e "${YELLOW}Completed $MAX_ITERATIONS iterations${NC}"
echo "No crash detected"
echo "========================================"
echo ""
echo "The crash might be rarer than expected. Options:"
echo "1. Increase MAX_ITERATIONS in the script"
echo "2. Check if crash conditions have changed"
echo "3. Review system logs for other clues"
