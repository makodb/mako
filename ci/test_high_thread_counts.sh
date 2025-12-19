#!/bin/bash
# Test high thread counts (12, 16) with CPU throttling
# Focused test for 2 shards with 12 and 16 threads per shard

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
cd "$PROJECT_ROOT"

CPU_LIMIT=10  # 10% CPU per worker thread
RUNTIME=50    # seconds per test (must exceed 30s benchmark default + load time)
LOG_DIR="/tmp/high_thread_test"

# Clean up
rm -rf "$LOG_DIR"
mkdir -p "$LOG_DIR"

echo "========================================="
echo "High Thread Count Throttling Test"
echo "========================================="
echo "CPU Limit: ${CPU_LIMIT}% per worker thread"
echo "Runtime: ${RUNTIME}s per test"
echo ""

cleanup() {
    pkill -9 -f dbtest 2>/dev/null || true
    rm -f /tmp/shuai_mako_rocksdb_shard* 2>/dev/null || true
    sleep 2
}

# Generate local shard list
gen_local_shards() {
    local n=$1
    local result=""
    for ((i=0; i<n; i++)); do
        if [ -n "$result" ]; then
            result="$result,$i"
        else
            result="$i"
        fi
    done
    echo "$result"
}

run_test() {
    local shards=$1
    local threads=$2
    local test_name="shards${shards}_threads${threads}"

    echo ""
    echo "--- Test: $shards shards, $threads threads/shard ---"

    cleanup

    local config="src/mako/config/local-shards${shards}-warehouses${threads}.yml"
    local local_shards=$(gen_local_shards $shards)
    local log_file="$LOG_DIR/${test_name}.log"

    if [ ! -f "$config" ]; then
        echo "  SKIP: Config $config not found"
        return 1
    fi

    # Run test
    timeout $((RUNTIME + 15)) ./build/dbtest \
        --num-threads $threads \
        --shard-config "$config" \
        -P localhost \
        -L "$local_shards" \
        --cpu-limit $CPU_LIMIT \
        2>&1 | tee "$log_file" &

    PID=$!
    sleep $RUNTIME
    kill $PID 2>/dev/null || true
    wait $PID 2>/dev/null || true

    # Check results
    if grep -q "agg_persist_throughput" "$log_file" 2>/dev/null; then
        local throughput=$(grep "agg_persist_throughput" "$log_file" | tail -1 | grep -oP '\d+(?= ops/sec)' || echo "0")
        if grep -q "PANIC\|SEGFAULT\|Assertion" "$log_file" 2>/dev/null; then
            echo "  FAIL: Crashes detected (throughput: $throughput)"
            return 1
        else
            echo "  PASS: throughput=$throughput ops/sec"
            return 0
        fi
    else
        echo "  FAIL: No throughput output"
        cat "$log_file" | tail -30
        return 1
    fi
}

# Track results
passed=0
failed=0

# Test 12 and 16 threads with 1-3 shards
for shards in 1 2 3; do
    for threads in 12 16; do
        if run_test $shards $threads; then
            passed=$((passed + 1))
        else
            failed=$((failed + 1))
        fi
    done
done

# Cleanup
cleanup

echo ""
echo "========================================="
echo "RESULTS SUMMARY"
echo "========================================="
echo "Passed: $passed"
echo "Failed: $failed"
echo ""

if [ $failed -gt 0 ]; then
    echo "Some tests failed. Check logs in $LOG_DIR"
    exit 1
else
    echo "All tests passed!"
    exit 0
fi
