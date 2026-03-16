#!/usr/bin/env bash
#
# Mako Correctness Test Suite - Master Runner
# =============================================
# Starts the makoCon server, runs all test suites, stops the server.
#
# Usage:
#   ./tests/correctness/run_all.sh          # Run all tests
#   ./tests/correctness/run_all.sh 1        # Run only Task 1
#   ./tests/correctness/run_all.sh 1 2 3    # Run Tasks 1, 2, 3
#

set -euo pipefail
cd "$(dirname "$0")/../.."
MAKO_ROOT="$(pwd)"
TEST_DIR="$MAKO_ROOT/tests/correctness"
RESULTS_FILE="$TEST_DIR/suite_results.txt"

echo "============================================================"
echo " Mako Correctness Test Suite"
echo " Root: $MAKO_ROOT"
echo " Commit: $(git rev-parse --short HEAD) ($(git log --oneline -1 | cut -d' ' -f2-))"
echo " Date: $(date -Iseconds)"
echo "============================================================"

# Check prerequisites
if [ ! -f "$MAKO_ROOT/build/makoCon" ]; then
    echo "ERROR: build/makoCon not found. Run 'make -j32' first."
    exit 1
fi

if ! python3 -c "import redis" 2>/dev/null; then
    echo "ERROR: Python redis module not found. Install with: pip3 install redis"
    exit 1
fi

# Determine which tasks to run
if [ $# -gt 0 ]; then
    TASKS="$@"
else
    # Tasks 4 and 16 restart the server; run them last to avoid destabilising
    # subsequent tests. Task 16 (crash workload) runs before task 4 (durability).
    TASKS="1 2 3 5 6 7 8 9 10 11 12 13 14 15 16 4"
fi

# Map task numbers to test files
declare -A TASK_FILES
TASK_FILES[1]="test_basic_kv.py"
TASK_FILES[2]="test_transactions.py"
TASK_FILES[3]="test_isolation.py"
TASK_FILES[4]="test_durability.py"
TASK_FILES[5]="test_stress.py"
TASK_FILES[6]="test_expanded.py"
TASK_FILES[7]="test_overwrite_investigation.py"
TASK_FILES[8]="test_overwrite_stress.py"
TASK_FILES[9]="test_delete.py"
TASK_FILES[10]="test_occ_benchmark.py"
TASK_FILES[11]="test_workload_bank.py"
TASK_FILES[12]="test_workload_sessions.py"
TASK_FILES[13]="test_workload_counter.py"
TASK_FILES[14]="test_workload_msgqueue.py"
TASK_FILES[15]="test_workload_inventory.py"
TASK_FILES[16]="test_workload_crash.py"

declare -A TASK_NAMES
TASK_NAMES[1]="Basic Key-Value Operations"
TASK_NAMES[2]="Transaction Operations"
TASK_NAMES[3]="Isolation and Concurrency"
TASK_NAMES[4]="Durability (Persistence)"
TASK_NAMES[5]="Stress and Edge Cases"
TASK_NAMES[6]="Expanded Coverage"
TASK_NAMES[7]="Overwrite Bug Investigation"
TASK_NAMES[8]="Overwrite Fix Stress Test"
TASK_NAMES[9]="Comprehensive Delete Testing"
TASK_NAMES[10]="OCC Conflict Rate Benchmark"
TASK_NAMES[11]="Workload: Bank Simulation"
TASK_NAMES[12]="Workload: Session Store"
TASK_NAMES[13]="Workload: Counter Service"
TASK_NAMES[14]="Workload: Message Queue"
TASK_NAMES[15]="Workload: Inventory Management"
TASK_NAMES[16]="Workload: Crash Recovery"

# Run tests
TOTAL_EXIT=0
> "$RESULTS_FILE"

for task in $TASKS; do
    file="${TASK_FILES[$task]:-}"
    name="${TASK_NAMES[$task]:-}"
    if [ -z "$file" ]; then
        echo "Unknown task: $task"
        continue
    fi

    # Kill leftover server between tasks to ensure clean state.
    # Each task calls ensure_server() to start a fresh instance.
    pkill -f "build/makoCon" 2>/dev/null || true
    sleep 1

    echo ""
    echo "============================================================"
    echo " Running Task $task: $name"
    echo "============================================================"

    if python3 "$TEST_DIR/$file" 2>&1 | tee -a "$RESULTS_FILE"; then
        echo "Task $task: PASSED" >> "$RESULTS_FILE"
    else
        echo "Task $task: FAILED" >> "$RESULTS_FILE"
        TOTAL_EXIT=1
    fi
done

# Kill any leftover makoCon processes we started
pkill -f "build/makoCon" 2>/dev/null || true

echo ""
echo "============================================================"
echo " SUITE COMPLETE"
echo " Results saved to: $RESULTS_FILE"
echo "============================================================"

# Print summary
echo ""
echo "--- Per-task results ---"
grep -E "^Task [0-9]+:" "$RESULTS_FILE" 2>/dev/null || echo "(no task-level results)"

echo ""
echo "--- Individual test results ---"
grep -E "^\[(PASS|FAIL)\]" "$RESULTS_FILE" | sort || echo "(no test results)"

PASS_COUNT=$(grep -cE "^\[PASS\]" "$RESULTS_FILE" 2>/dev/null || echo 0)
FAIL_COUNT=$(grep -cE "^\[FAIL\]" "$RESULTS_FILE" 2>/dev/null || echo 0)
echo ""
echo "Total: $PASS_COUNT passed, $FAIL_COUNT failed"

exit $TOTAL_EXIT
