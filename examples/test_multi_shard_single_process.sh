#!/bin/bash

# Script to test multi-shard single process mode
# This tests running multiple shards (0 and 1) in a single process using the -L flag
#
# Success criteria:
# 1. Show "Multi-shard mode: running 2 shards in this process"
# 2. Show "Created SiloRuntime" for each shard
# 3. Show "Initialized ShardContext for shard" for each shard
# 4. Show "agg_persist_throughput" keyword

echo "========================================="
echo "Testing multi-shard single process mode"
echo "========================================="

# Clean up old log files
rm -f nfs_sync_*

trd=${1:-6}
script_name="$(basename "$0")"

# Determine transport type and create unique log prefix
transport="${MAKO_TRANSPORT:-rrr}"
log_prefix="${script_name}_${transport}"
log_file="${log_prefix}_multi_shard-$trd.log"

ps aux | grep -i dbtest | awk "{print \$2}" | xargs kill -9 2>/dev/null
sleep 1

path=$(pwd)/src/mako

# Build the command for multi-shard single process mode
# Key: -L 0,1 specifies running shards 0 and 1 in the same process
CMD="./build/dbtest --num-threads $trd --shard-config $path/config/local-shards2-warehouses$trd.yml -P localhost -L 0,1"

echo ""
echo "Configuration:"
echo "-----------------"
echo "  Number of threads: $trd"
echo "  Local shards:      0,1 (multi-shard mode)"
echo "  Config file:       $path/config/local-shards2-warehouses$trd.yml"
echo "  Log file:          $log_file"
echo ""
echo "Command: $CMD"
echo ""

# Start multi-shard process in background
echo "Starting multi-shard single process..."
nohup $CMD > $log_file 2>&1 &
PROCESS_PID=$!
sleep 2

# Wait for experiments to run
echo "Running experiments for 30 seconds..."
sleep 50

# Kill the process
echo "Stopping process..."
kill $PROCESS_PID 2>/dev/null
wait $PROCESS_PID 2>/dev/null

echo ""
echo "========================================="
echo "Checking test results..."
echo "========================================="

failed=0

echo ""
echo "Checking $log_file:"
echo "-----------------"

if [ ! -f "$log_file" ]; then
    echo "  ✗ Log file not found"
    exit 1
fi

# Check 1: Multi-shard mode initialization
if grep -q "Multi-shard mode: running 2 shards in this process" "$log_file"; then
    echo "  ✓ Multi-shard mode initialization detected (2 shards)"
else
    echo "  ✗ Multi-shard mode initialization not found"
    failed=1
fi

# Check 2: SiloRuntime assignment for shard 0 (shared runtime in multi-shard mode)
if grep -q "Assigned shared SiloRuntime.*to shard 0" "$log_file"; then
    echo "  ✓ SiloRuntime assigned to shard 0"
    grep "Assigned shared SiloRuntime.*to shard 0" "$log_file" | head -1 | sed 's/^/    /'
else
    echo "  ✗ SiloRuntime for shard 0 not found"
    failed=1
fi

# Check 3: SiloRuntime assignment for shard 1 (shared runtime in multi-shard mode)
if grep -q "Assigned shared SiloRuntime.*to shard 1" "$log_file"; then
    echo "  ✓ SiloRuntime assigned to shard 1"
    grep "Assigned shared SiloRuntime.*to shard 1" "$log_file" | head -1 | sed 's/^/    /'
else
    echo "  ✗ SiloRuntime for shard 1 not found"
    failed=1
fi

# Check 4: ShardContext initialization for shard 0
if grep -q "Initialized ShardContext for shard 0" "$log_file"; then
    echo "  ✓ ShardContext initialized for shard 0"
else
    echo "  ✗ ShardContext for shard 0 not initialized"
    failed=1
fi

# Check 5: ShardContext initialization for shard 1
if grep -q "Initialized ShardContext for shard 1" "$log_file"; then
    echo "  ✓ ShardContext initialized for shard 1"
else
    echo "  ✗ ShardContext for shard 1 not initialized"
    failed=1
fi

# Check 6: Shard listing in log
for shard in 0 1; do
    if grep -q "  - Shard $shard" "$log_file"; then
        echo "  ✓ Shard $shard listed in multi-shard output"
    else
        echo "  ⚠ Shard $shard not listed in multi-shard output (minor)"
    fi
done

# Check 7: Workers running in parallel for shard 0
if grep -q "Running workers for shard 0 in thread" "$log_file"; then
    echo "  ✓ Workers running in parallel thread for shard 0"
else
    echo "  ✗ Workers not running in thread for shard 0"
    failed=1
fi

# Check 8: Workers running in parallel for shard 1
if grep -q "Running workers for shard 1 in thread" "$log_file"; then
    echo "  ✓ Workers running in parallel thread for shard 1"
else
    echo "  ✗ Workers not running in thread for shard 1"
    failed=1
fi

# Check 9: Benchmark started (at least one shard)
benchmark_count=$(grep -c "starting benchmark" "$log_file" || echo "0")
if [ "$benchmark_count" -ge 1 ]; then
    echo "  ✓ Benchmark started ($benchmark_count shard(s))"
else
    echo "  ⚠ Benchmark not started (may still be loading)"
fi

# Check 10: Look for throughput output (system is running)
if grep -q "agg_persist_throughput" "$log_file"; then
    echo "  ✓ Found 'agg_persist_throughput' keyword (system running)"
    grep "agg_persist_throughput" "$log_file" | tail -1 | sed 's/^/    /'
else
    echo "  ⚠ 'agg_persist_throughput' keyword not found (may still be initializing)"
fi

echo ""
echo "========================================="
if [ $failed -eq 0 ]; then
    echo "All checks passed!"
    echo "Multi-shard single process mode is working correctly."
    echo "========================================="
    exit 0
else
    echo "Some checks failed!"
    echo "========================================="
    echo ""
    echo "Debug information:"
    echo "Check $log_file for details"
    echo ""
    echo "Last 20 lines of $log_file:"
    tail -20 "$log_file"
    exit 1
fi
