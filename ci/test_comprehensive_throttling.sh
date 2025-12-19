#!/bin/bash
# Comprehensive single-process throttling test
# Tests various combinations of shards, threads, and replication modes

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
cd "$PROJECT_ROOT"

CPU_LIMIT=10  # 10% CPU per worker thread
RUNTIME=40    # seconds per test (must exceed 30s benchmark default + load time)
LOG_DIR="/tmp/comprehensive_throttling_test"

# Clean up
rm -rf "$LOG_DIR"
mkdir -p "$LOG_DIR"

echo "========================================="
echo "Comprehensive Single-Process Throttling Test"
echo "========================================="
echo "CPU Limit: ${CPU_LIMIT}% per worker thread"
echo "Runtime: ${RUNTIME}s per test"
echo ""

# Results tracking
declare -A results
test_num=0
passed=0
failed=0

cleanup() {
    pkill -9 -f dbtest 2>/dev/null || true
    pkill -9 -f "bash/shard.sh" 2>/dev/null || true
    rm -f /tmp/shuai_mako_rocksdb_shard* 2>/dev/null || true
    rm -f nfs_sync_* 2>/dev/null || true
    sleep 2
}

# Generate local shard list: "0" for 1 shard, "0,1" for 2 shards, etc.
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

run_test_no_replication() {
    local shards=$1
    local threads=$2
    local test_name="shards${shards}_threads${threads}_norepl"

    test_num=$((test_num + 1))
    echo ""
    echo "--- Test $test_num: $shards shards, $threads threads/shard, NO replication ---"

    cleanup

    local config="src/mako/config/local-shards${shards}-warehouses${threads}.yml"
    local local_shards=$(gen_local_shards $shards)
    local log_file="$LOG_DIR/${test_name}.log"

    if [ ! -f "$config" ]; then
        echo "  SKIP: Config $config not found"
        results[$test_name]="SKIP"
        return
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
            results[$test_name]="FAIL"
            failed=$((failed + 1))
        else
            echo "  PASS: throughput=$throughput ops/sec"
            results[$test_name]="PASS:$throughput"
            passed=$((passed + 1))
        fi
    else
        echo "  FAIL: No throughput output"
        results[$test_name]="FAIL"
        failed=$((failed + 1))
    fi
}

run_test_with_replication() {
    local shards=$1
    local threads=$2
    local test_name="shards${shards}_threads${threads}_repl"

    test_num=$((test_num + 1))
    echo ""
    echo "--- Test $test_num: $shards shards, $threads threads/shard, WITH replication ---"

    cleanup

    local config="src/mako/config/local-shards${shards}-warehouses${threads}.yml"
    local local_shards=$(gen_local_shards $shards)
    local log_file="$LOG_DIR/${test_name}.log"

    if [ ! -f "$config" ]; then
        echo "  SKIP: Config $config not found"
        results[$test_name]="SKIP"
        return
    fi

    # Check paxos configs exist for all shards
    for ((i=0; i<shards; i++)); do
        local paxos_cfg="config/1leader_2followers/paxos${threads}_shardidx${i}.yml"
        if [ ! -f "$paxos_cfg" ]; then
            echo "  SKIP: Paxos config $paxos_cfg not found"
            results[$test_name]="SKIP"
            return
        fi
    done

    # Start follower processes for each shard
    echo "  Starting follower processes..."
    for ((i=0; i<shards; i++)); do
        nohup bash bash/shard.sh $shards $i $threads learner 0 1 > "$LOG_DIR/${test_name}_shard${i}_learner.log" 2>&1 &
        nohup bash bash/shard.sh $shards $i $threads p2 0 1 > "$LOG_DIR/${test_name}_shard${i}_p2.log" 2>&1 &
    done
    sleep 1

    for ((i=0; i<shards; i++)); do
        nohup bash bash/shard.sh $shards $i $threads p1 0 1 > "$LOG_DIR/${test_name}_shard${i}_p1.log" 2>&1 &
    done
    sleep 3

    # Build paxos config flags
    local paxos_flags=""
    for ((i=0; i<shards; i++)); do
        paxos_flags="$paxos_flags -F config/1leader_2followers/paxos${threads}_shardidx${i}.yml"
    done

    # Start combined leader with replication
    echo "  Starting combined leader..."
    timeout $((RUNTIME + 30)) ./build/dbtest \
        --num-threads $threads \
        --shard-config "$config" \
        $paxos_flags \
        -F config/occ_paxos.yml \
        -P localhost \
        -L "$local_shards" \
        --cpu-limit $CPU_LIMIT \
        --is-replicated \
        2>&1 | tee "$log_file" &

    LEADER_PID=$!

    # Wait for completion or timeout
    local wait_count=0
    while [ $wait_count -lt $((RUNTIME + 20)) ]; do
        if grep -q "agg_persist_throughput" "$log_file" 2>/dev/null; then
            sleep 2
            break
        fi
        sleep 1
        wait_count=$((wait_count + 1))
    done

    # Cleanup
    kill $LEADER_PID 2>/dev/null || true
    pkill -9 -f "bash/shard.sh" 2>/dev/null || true
    pkill -9 dbtest 2>/dev/null || true
    sleep 2

    # Check results
    if grep -q "agg_persist_throughput" "$log_file" 2>/dev/null; then
        local throughput=$(grep "agg_persist_throughput" "$log_file" | tail -1 | grep -oP '\d+(?= ops/sec)' || echo "0")
        if grep -q "PANIC\|SEGFAULT\|Assertion\|IT should never happen" "$log_file" 2>/dev/null; then
            echo "  FAIL: Crashes/panics detected (throughput: $throughput)"
            results[$test_name]="FAIL"
            failed=$((failed + 1))
        else
            echo "  PASS: throughput=$throughput ops/sec"
            results[$test_name]="PASS:$throughput"
            passed=$((passed + 1))
        fi
    else
        echo "  FAIL: No throughput output"
        results[$test_name]="FAIL"
        failed=$((failed + 1))
    fi
}

# ============================================
# Test Matrix
# ============================================
# Shards: 1, 2, 3, 4, 5
# Threads per shard: 1, 2, 4, 6, 8, 12, 16
# Replication: yes/no
#
# Memory estimation: ~500MB base + ~100MB per shard + ~50MB per thread
# With 62GB RAM, we can safely test up to 5 shards x 16 threads
# ============================================

echo ""
echo "========================================="
echo "Phase 1: Tests WITHOUT Replication"
echo "========================================="

# Test without replication - wider range
for shards in 1 2 3 4 5; do
    for threads in 1 2 4 6 8 12 16; do
        run_test_no_replication $shards $threads
    done
done

echo ""
echo "========================================="
echo "Phase 2: Tests WITH Replication"
echo "========================================="

# Test with replication - more selective (replication adds overhead)
# Using threads that have paxos configs (typically 1-16 in increments)
for shards in 1 2 3; do
    for threads in 1 2 4 6 8; do
        run_test_with_replication $shards $threads
    done
done

# Cleanup
cleanup

# ============================================
# Results Summary
# ============================================
echo ""
echo "========================================="
echo "RESULTS SUMMARY"
echo "========================================="
echo ""
printf "%-40s %s\n" "Test" "Result"
printf "%-40s %s\n" "----" "------"

for key in $(echo "${!results[@]}" | tr ' ' '\n' | sort); do
    printf "%-40s %s\n" "$key" "${results[$key]}"
done

echo ""
echo "========================================="
echo "Total: $test_num tests"
echo "Passed: $passed"
echo "Failed: $failed"
echo "========================================="

if [ $failed -gt 0 ]; then
    echo ""
    echo "Some tests failed. Check logs in $LOG_DIR"
    exit 1
else
    echo ""
    echo "All tests passed!"
    exit 0
fi
