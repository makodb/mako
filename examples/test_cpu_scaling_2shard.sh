#!/bin/bash

# Script to test CPU scaling for 2-shard single-process mode WITHOUT replication
# Tests throughput at different CPU cap levels (5%, 10%, 20%, 40%)
# Uses cgroups v2 via systemd-run for CPU limiting
#
# Configuration:
# - 2 shards in single process (-L 0,1)
# - 6 worker threads per shard (12 total)
# - No replication
#
# Success criteria:
# - Throughput should scale roughly linearly with CPU cap increase

set -e

echo "========================================="
echo "CPU Scaling Test: 2-shard single process"
echo "========================================="
echo ""

# Configuration
NUM_THREADS=6  # Use 6 threads to avoid TPC-C workload crashes with high thread counts
NUM_CPUS=$(nproc)

# Wait times - need to account for DB loading which is slower with CPU caps
# Lower CPU caps need longer wait times for loading + benchmark (30s runtime)
WAIT_5PCT=180    # 3 minutes for 5% (loading is very slow)
WAIT_10PCT=120   # 2 minutes for 10%
WAIT_20PCT=90    # 1.5 minutes for 20%
WAIT_40PCT=60    # 1 minute for 40%
WAIT_UNLIMITED=60 # 1 minute for unlimited

# CPU caps to test (as percentage of total system CPU)
# Add unlimited (100) as baseline
CPU_CAPS="5 10 20 40 100"

# Clean up any existing processes
cleanup() {
    echo "Cleaning up..."
    pkill -f "dbtest.*-L 0,1" 2>/dev/null || true
    sleep 1
}

trap cleanup EXIT

# Clean up old log files
rm -f nfs_sync_*
rm -f cpu_scaling_*.log
USERNAME=${USER:-unknown}
rm -rf /tmp/${USERNAME}_mako_rocksdb_shard*

# Kill any existing dbtest processes
pkill -f dbtest 2>/dev/null || true
sleep 2

path=$(pwd)/src/mako

echo "System Configuration:"
echo "---------------------"
echo "  Number of CPUs:      $NUM_CPUS"
echo "  Worker threads/shard: $NUM_THREADS"
echo "  Total worker threads: $((NUM_THREADS * 2))"
echo "  Config file:         $path/config/local-shards2-warehouses${NUM_THREADS}.yml"
echo ""
echo "CPU caps to test: 5%, 10%, 20%, 40%, unlimited (100%)"
echo "Wait times: 5%=${WAIT_5PCT}s, 10%=${WAIT_10PCT}s, 20%=${WAIT_20PCT}s, 40%=${WAIT_40PCT}s, 100%=${WAIT_UNLIMITED}s"
echo ""

# Check if config file exists
if [ ! -f "$path/config/local-shards2-warehouses${NUM_THREADS}.yml" ]; then
    echo "ERROR: Config file not found: $path/config/local-shards2-warehouses${NUM_THREADS}.yml"
    exit 1
fi

# Check if dbtest exists
if [ ! -f "./build/dbtest" ]; then
    echo "ERROR: ./build/dbtest not found. Please build first."
    exit 1
fi

# Check if we can use systemd-run (requires cgroups)
if ! command -v systemd-run &> /dev/null; then
    echo "ERROR: systemd-run not available. Cannot use cgroups for CPU limiting."
    exit 1
fi

# Results storage
declare -A THROUGHPUT_RESULTS

# Function to extract throughput from log
extract_throughput() {
    local log_file=$1
    # Look for agg_persist_throughput line and extract the value
    local throughput=$(grep "agg_persist_throughput" "$log_file" 2>/dev/null | tail -1 | awk '{print $2}')
    if [ -z "$throughput" ]; then
        throughput="N/A"
    fi
    echo "$throughput"
}

# Get wait time for a given CPU cap
get_wait_time() {
    local cpu_percent=$1
    case $cpu_percent in
        5)   echo $WAIT_5PCT ;;
        10)  echo $WAIT_10PCT ;;
        20)  echo $WAIT_20PCT ;;
        40)  echo $WAIT_40PCT ;;
        100) echo $WAIT_UNLIMITED ;;
        *)   echo 90 ;;  # Default
    esac
}

# Function to run test with CPU cap
run_test_with_cpu_cap() {
    local cpu_percent=$1
    local log_file="cpu_scaling_${cpu_percent}pct.log"
    local max_wait=$(get_wait_time $cpu_percent)

    # Clean up before test
    pkill -f "dbtest.*-L 0,1" 2>/dev/null || true
    rm -rf /tmp/${USERNAME}_mako_rocksdb_shard*
    sleep 2

    # Build command
    local CMD="./build/dbtest --num-threads $NUM_THREADS --shard-config $path/config/local-shards2-warehouses${NUM_THREADS}.yml -P localhost -L 0,1"

    echo ""
    echo "========================================="
    if [ "$cpu_percent" -eq 100 ]; then
        echo "Testing with UNLIMITED CPU (baseline)"
        echo "========================================="
        echo "Starting dbtest without CPU limit..."
        echo "Command: $CMD"

        # Run without CPU limit
        $CMD > "$log_file" 2>&1 &
    else
        # Convert system percentage to cgroups CPUQuota (100% = 1 CPU core)
        # For total system percentage: cpu_percent% of NUM_CPUS cores
        local cpu_quota=$((cpu_percent * NUM_CPUS))

        echo "Testing with ${cpu_percent}% CPU cap (CPUQuota=${cpu_quota}%)"
        echo "========================================="
        echo "Starting dbtest with CPU limit..."
        echo "Command: systemd-run --user --scope -p CPUQuota=${cpu_quota}% $CMD"

        # Run with CPU limit using systemd-run
        systemd-run --user --scope -p CPUQuota=${cpu_quota}% $CMD > "$log_file" 2>&1 &
    fi

    local PID=$!
    echo "Process started with PID $PID"
    echo "Waiting for benchmark to complete (max ${max_wait}s)..."

    local wait_count=0
    local completed=0

    while [ $wait_count -lt $max_wait ]; do
        # Check if process is still running
        if ! kill -0 $PID 2>/dev/null; then
            echo "Process exited at ${wait_count}s"
            completed=1
            break
        fi

        # Check for throughput output (printed at end of benchmark)
        if grep -q "agg_persist_throughput" "$log_file" 2>/dev/null; then
            echo "Benchmark completed at ${wait_count}s (found throughput output)"
            completed=1
            sleep 2  # Give it time to finish writing
            break
        fi

        sleep 1
        wait_count=$((wait_count + 1))

        if [ $((wait_count % 15)) -eq 0 ]; then
            echo "  ... ${wait_count}s elapsed"
            # Show progress
            if grep -q "runtime time left" "$log_file" 2>/dev/null; then
                grep "runtime time left" "$log_file" 2>/dev/null | tail -1 | sed 's/^.*runtime/    runtime/'
            fi
        fi
    done

    # Stop the process if still running
    if kill -0 $PID 2>/dev/null; then
        echo "Stopping process (timeout)..."
        kill $PID 2>/dev/null || true
        wait $PID 2>/dev/null || true
    fi
    sleep 1

    # Extract throughput
    local throughput=$(extract_throughput "$log_file")
    THROUGHPUT_RESULTS[$cpu_percent]=$throughput

    echo ""
    echo "Result for ${cpu_percent}% CPU cap:"
    echo "  Throughput: $throughput txn/s"

    # Show throughput line for verification
    if [ "$throughput" != "N/A" ]; then
        echo "  Full output:"
        grep "agg_persist_throughput" "$log_file" 2>/dev/null | sed 's/^/    /'
    fi

    return 0
}

# Run tests for each CPU cap
echo ""
echo "Starting CPU scaling tests..."
echo ""

for cap in $CPU_CAPS; do
    run_test_with_cpu_cap $cap
done

# Print summary
echo ""
echo "========================================="
echo "CPU SCALING TEST RESULTS"
echo "========================================="
echo ""
printf "%-15s %-20s\n" "CPU Cap (%)" "Throughput (txn/s)"
echo "-----------------------------------"

prev_throughput=""
for cap in $CPU_CAPS; do
    throughput=${THROUGHPUT_RESULTS[$cap]}
    scaling=""

    # Calculate scaling factor compared to previous
    if [ -n "$prev_throughput" ] && [ "$prev_throughput" != "N/A" ] && [ "$throughput" != "N/A" ]; then
        # Use awk for floating point division
        scaling=$(awk "BEGIN {printf \"%.2fx\", $throughput / $prev_throughput}")
    fi

    # Format cap label
    if [ "$cap" -eq 100 ]; then
        cap_label="unlimited"
    else
        cap_label="${cap}%"
    fi

    printf "%-15s %-20s %s\n" "$cap_label" "$throughput" "$scaling"
    prev_throughput=$throughput
done

echo ""
echo "-----------------------------------"
echo ""

# Analyze scaling
echo "Scaling Analysis:"
echo "-----------------"

# Get first and last throughput for overall scaling
first_cap=$(echo $CPU_CAPS | awk '{print $1}')
last_cap=$(echo $CPU_CAPS | awk '{print $NF}')
first_tp=${THROUGHPUT_RESULTS[$first_cap]}
last_tp=${THROUGHPUT_RESULTS[$last_cap]}

if [ "$first_tp" != "N/A" ] && [ "$last_tp" != "N/A" ]; then
    # Calculate expected vs actual scaling
    expected_scaling=$(awk "BEGIN {printf \"%.1f\", $last_cap / $first_cap}")
    actual_scaling=$(awk "BEGIN {printf \"%.2f\", $last_tp / $first_tp}")
    efficiency=$(awk "BEGIN {printf \"%.1f\", ($actual_scaling / $expected_scaling) * 100}")

    echo "  CPU increased: ${first_cap}% -> ${last_cap}% (${expected_scaling}x)"
    echo "  Throughput:    $first_tp -> $last_tp (${actual_scaling}x)"
    echo "  Scaling efficiency: ${efficiency}%"
    echo ""

    if awk "BEGIN {exit !($actual_scaling >= $expected_scaling * 0.7)}"; then
        echo "  RESULT: GOOD - Throughput scales well with CPU (>70% efficiency)"
    elif awk "BEGIN {exit !($actual_scaling >= $expected_scaling * 0.5)}"; then
        echo "  RESULT: MODERATE - Throughput scales moderately with CPU (50-70% efficiency)"
    else
        echo "  RESULT: POOR - Throughput does not scale well with CPU (<50% efficiency)"
    fi
else
    echo "  Could not calculate scaling (missing throughput data)"
fi

echo ""
echo "Log files: cpu_scaling_*.log"
echo "========================================="
