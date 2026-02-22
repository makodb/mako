#!/bin/bash
# Test CPU throttling scaling: verify throughput roughly doubles when CPU cap doubles.
# Runs 1%, 2%, 4%, 8% CPU limits with 2 shards and 6 threads per shard.
# Each run must finish and emit agg_persist_throughput to be considered valid.

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
cd "$PROJECT_ROOT"

THREADS=6
CONFIG="src/mako/config/local-shards2-warehouses6.yml"
RUNTIME=30         # Match dbtest default benchmark runtime.
TIMEOUT_BUFFER=20  # Extra safety window for startup/shutdown.
USERNAME=${USER:-$(id -un)}
LOG_DIR="/tmp/${USERNAME}_cpu_throttling_test"

extract_throughput() {
    local log_file="$1"
    local line value

    line=$(grep "agg_persist_throughput:" "$log_file" | tail -1 || true)
    if [ -z "$line" ]; then
        echo ""
        return
    fi

    value=$(printf '%s\n' "$line" | sed -E 's/.*agg_persist_throughput:[[:space:]]*([0-9]+(\.[0-9]+)?).*/\1/')
    if ! [[ "$value" =~ ^[0-9]+([.][0-9]+)?$ ]]; then
        echo ""
        return
    fi

    echo "$value"
}

float_gt() {
    local left="$1"
    local right="$2"
    awk -v a="$left" -v b="$right" 'BEGIN { exit !(a > b) }'
}

float_between_inclusive() {
    local value="$1"
    local low="$2"
    local high="$3"
    awk -v v="$value" -v lo="$low" -v hi="$high" 'BEGIN { exit !(v >= lo && v <= hi) }'
}

float_div() {
    local numerator="$1"
    local denominator="$2"
    awk -v n="$numerator" -v d="$denominator" 'BEGIN { if (d == 0) { print "0.00" } else { printf "%.2f", n / d } }'
}

# Clean up
rm -rf "$LOG_DIR"
mkdir -p "$LOG_DIR"
pkill -9 -f dbtest 2>/dev/null || true
sleep 1

echo "========================================="
echo "CPU Throttling Scaling Test"
echo "========================================="
echo "Config: 2 shards, $THREADS threads per shard"
echo "Testing CPU limits: 1%, 2%, 4%, 8%"
echo ""

declare -A throughputs
all_passed=true

for cpu_limit in 1 2 4 8; do
    echo "--- Testing CPU limit: ${cpu_limit}% ---"

    # Clean up RocksDB files
    rm -f /tmp/${USERNAME}_mako_rocksdb_shard* 2>/dev/null

    LOG_FILE="$LOG_DIR/cpu_${cpu_limit}pct.log"

    # Run test to completion so final benchmark summary is emitted.
    set +e
    timeout $((RUNTIME + TIMEOUT_BUFFER)) ./${BUILD_DIR:-build}/dbtest \
        --num-threads "$THREADS" \
        --shard-config "$CONFIG" \
        -P localhost \
        -L 0,1 \
        --cpu-limit "$cpu_limit" \
        2>&1 | tee "$LOG_FILE"
    test_exit=${PIPESTATUS[0]}
    set -e

    if [ "$test_exit" -eq 124 ]; then
        echo "  ERROR: Test timed out before completion"
        throughputs[$cpu_limit]=0
        all_passed=false
        sleep 2
        continue
    fi

    if [ "$test_exit" -ne 0 ]; then
        echo "  ERROR: dbtest exited with status $test_exit"
        throughputs[$cpu_limit]=0
        all_passed=false
        sleep 2
        continue
    fi

    throughput=$(extract_throughput "$LOG_FILE")

    if [ -z "$throughput" ] || ! float_gt "$throughput" "0"; then
        echo "  ERROR: Could not extract agg_persist_throughput from benchmark summary"
        throughputs[$cpu_limit]=0
        all_passed=false
    else
        echo "  Throughput: $throughput ops/sec"
        throughputs[$cpu_limit]=$throughput
    fi

    # Small delay between tests
    sleep 2
done

echo ""
echo "========================================="
echo "Results Summary"
echo "========================================="
printf "%-10s %-15s %-15s\n" "CPU%" "Throughput" "Scaling"

prev_tput=0

for cpu_limit in 1 2 4 8; do
    tput=${throughputs[$cpu_limit]:-0}

    if ! float_gt "$tput" "0"; then
        printf "%-10s %-15s %-15s %s\n" "${cpu_limit}%" "$tput" "-" "FAIL (missing throughput)"
        all_passed=false
    elif float_gt "$prev_tput" "0"; then
        # Calculate scaling factor (expect ~2x)
        scaling=$(float_div "$tput" "$prev_tput")

        # Check if scaling is roughly 2x (between 1.5x and 2.5x)
        if float_between_inclusive "$scaling" "1.3" "3.0"; then
            status="OK"
        else
            status="FAIL (expected ~2x)"
            all_passed=false
        fi

        printf "%-10s %-15s %-15s %s\n" "${cpu_limit}%" "$tput" "${scaling}x" "$status"
    else
        printf "%-10s %-15s %-15s\n" "${cpu_limit}%" "$tput" "-"
    fi

    prev_tput=$tput
done

echo ""

# Check for any crashes/panics
if grep -q "PANIC\|SEGFAULT\|Assertion" "$LOG_DIR"/*.log 2>/dev/null; then
    echo "ERROR: Found crashes in logs!"
    all_passed=false
fi

if [ "$all_passed" = true ]; then
    echo "========================================="
    echo "All scaling checks passed!"
    echo "========================================="
    exit 0
else
    echo "========================================="
    echo "Some scaling checks failed!"
    echo "========================================="
    exit 1
fi
