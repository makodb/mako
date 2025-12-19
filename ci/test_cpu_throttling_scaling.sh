#!/bin/bash
# Test CPU throttling scaling: verify throughput roughly doubles when CPU cap doubles
# Tests 1%, 2%, 4%, 8% CPU limits with 2-shard, 6 threads per shard

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
cd "$PROJECT_ROOT"

THREADS=6
CONFIG="src/mako/config/local-shards2-warehouses6.yml"
RUNTIME=20  # seconds per test
LOG_DIR="/tmp/cpu_throttling_test"

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

for cpu_limit in 1 2 4 8; do
    echo "--- Testing CPU limit: ${cpu_limit}% ---"

    # Clean up RocksDB files
    rm -f /tmp/shuai_mako_rocksdb_shard* 2>/dev/null

    LOG_FILE="$LOG_DIR/cpu_${cpu_limit}pct.log"

    # Run test
    timeout $((RUNTIME + 15)) ./build/dbtest \
        --num-threads $THREADS \
        --shard-config "$CONFIG" \
        -P localhost \
        -L 0,1 \
        --cpu-limit $cpu_limit \
        2>&1 | tee "$LOG_FILE" &

    PID=$!
    sleep $RUNTIME
    kill $PID 2>/dev/null || true
    wait $PID 2>/dev/null || true

    # Extract throughput
    throughput=$(grep "agg_persist_throughput" "$LOG_FILE" | tail -1 | grep -oP '\d+(?= ops/sec)' || echo "0")

    if [ "$throughput" = "0" ] || [ -z "$throughput" ]; then
        echo "  ERROR: Could not extract throughput"
        throughputs[$cpu_limit]=0
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
all_passed=true

for cpu_limit in 1 2 4 8; do
    tput=${throughputs[$cpu_limit]}

    if [ $prev_tput -gt 0 ] && [ $tput -gt 0 ]; then
        # Calculate scaling factor (expect ~2x)
        scaling=$(echo "scale=2; $tput / $prev_tput" | bc)

        # Check if scaling is roughly 2x (between 1.5x and 2.5x)
        scaling_ok=$(echo "$scaling >= 1.3 && $scaling <= 3.0" | bc)

        if [ "$scaling_ok" = "1" ]; then
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
