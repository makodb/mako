#!/bin/bash

# Script to test 2-shard single process mode WITH replication
# This runs both shard leaders (0 and 1) in a single process using -L flag
# while still having separate follower processes for each shard
#
# Process layout:
# - 1 process: Leader for shards 0 and 1 (combined using -L 0,1)
# - 6 processes: Followers (p1, p2, learner) x 2 shards
# Total: 7 processes (reduced from 8 in multi-process mode)
#
# Success criteria:
# 1. Show "agg_persist_throughput" keyword
# 2. System completes without transaction aborts

# Source common utilities (includes GDB_PREFIX for debugging)
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
source "${SCRIPT_DIR}/../bash/util.sh"
source "${SCRIPT_DIR}/simple_transaction_rep_port_utils.sh"

echo "========================================="
echo "Testing 2-shard single process mode WITH replication"
echo "========================================="

if [ "$GDB_ENABLED" == "1" ]; then
    echo "[GDB] Debug mode enabled"
fi

# Clean up old log files
rm -f nfs_sync_*
USERNAME=${USER:-unknown}
rm -rf /tmp/${USERNAME}_mako_rocksdb_shard*

trd=${1:-${MAKO_CI_TRD:-6}}
script_name="$(basename "$0")"
binary_path="./${BUILD_DIR:-build}/dbtest"

if [ ! -x "$binary_path" ]; then
    echo "Error: dbtest binary not found or not executable at '$binary_path'"
    echo "Build it first (for Docker: ./docker_build.sh build), then retry."
    exit 1
fi

if ! ensure_paxos_replication_configs "$trd" 2; then
    exit 1
fi

TEMP_CONFIG=$(make_simple_txn_rep_config 2 "$trd")
if [ -z "$TEMP_CONFIG" ]; then
    exit 1
fi
export MAKO_CONFIG="$TEMP_CONFIG"
config_path="$MAKO_CONFIG"
echo "dbtest config: $config_path"

# Randomize paxos replication ports — see test_2shard_replication.sh.
TEMP_PAXOS_DIR=$(make_paxos_replication_configs 2 "$trd" paxos)
if [ -z "$TEMP_PAXOS_DIR" ]; then
    exit 1
fi
export MAKO_PAXOS_CONFIG_DIR="$TEMP_PAXOS_DIR"
echo "paxos replication config dir: $MAKO_PAXOS_CONFIG_DIR"

LEADER_PID=""
SHARD0_LEARNER_PID=""
SHARD0_P2_PID=""
SHARD0_P1_PID=""
SHARD1_LEARNER_PID=""
SHARD1_P2_PID=""
SHARD1_P1_PID=""
CLEANUP_DONE=0

# Determine transport type and create unique log prefix
transport="rrr"
log_prefix="${script_name}_${transport}"

# Kill only dbtest worker processes by executable name.
# Avoid grep/xargs patterns that can match wrapper shells containing "dbtest" in argv.
pkill -9 -x dbtest 2>/dev/null || true
sleep 1

# This test launches many replicated processes and RocksDB instances.
# Raise nofile for this shell so all child dbtest processes inherit it.
target_nofile=65535
hard_nofile=$(ulimit -Hn 2>/dev/null || echo "")
if [ -n "$hard_nofile" ] && [ "$hard_nofile" != "unlimited" ] && [ "$hard_nofile" -lt "$target_nofile" ]; then
    target_nofile="$hard_nofile"
fi
ulimit -n "$target_nofile" 2>/dev/null || true
current_nofile=$(ulimit -n 2>/dev/null || echo "unknown")

max_wait="${MAKO_MAX_WAIT_SECONDS:-120}"
if ! [[ "$max_wait" =~ ^[0-9]+$ ]] || [ "$max_wait" -le 0 ]; then
    echo "Warning: MAKO_MAX_WAIT_SECONDS='${max_wait}' is invalid; using default 120s"
    max_wait=120
fi

follower_ready_timeout="${MAKO_FOLLOWER_READY_TIMEOUT_SEC:-60}"
if ! [[ "$follower_ready_timeout" =~ ^[0-9]+$ ]] || [ "$follower_ready_timeout" -le 0 ]; then
    echo "Warning: MAKO_FOLLOWER_READY_TIMEOUT_SEC='${follower_ready_timeout}' is invalid; using default 60s"
    follower_ready_timeout=60
fi

leader_startup_timeout="${MAKO_STARTUP_TIMEOUT_SEC:-$((max_wait + 60))}"
if ! [[ "$leader_startup_timeout" =~ ^[0-9]+$ ]] || [ "$leader_startup_timeout" -le 0 ]; then
    echo "Warning: MAKO_STARTUP_TIMEOUT_SEC='${leader_startup_timeout}' is invalid; using default $((max_wait + 60))s"
    leader_startup_timeout=$((max_wait + 60))
fi

wait_for_log_pattern() {
    local log_file="$1"
    local pattern="$2"
    local label="$3"
    local timeout_sec="$4"
    local waited=0

    while [ "$waited" -lt "$timeout_sec" ]; do
        if [ -f "$log_file" ] && grep -q "$pattern" "$log_file" 2>/dev/null; then
            echo "  Ready: $label (${waited}s)"
            return 0
        fi
        sleep 1
        waited=$((waited + 1))
    done

    echo "  X Timeout waiting for $label marker '$pattern' in $log_file after ${timeout_sec}s"
    return 1
}

cleanup_processes() {
    if [ "$CLEANUP_DONE" -eq 1 ]; then
        return
    fi
    CLEANUP_DONE=1

    # Stop started wrapper/leader processes first.
    for pid in "${LEADER_PID:-}" "${SHARD0_LEARNER_PID:-}" "${SHARD0_P2_PID:-}" "${SHARD0_P1_PID:-}" \
               "${SHARD1_LEARNER_PID:-}" "${SHARD1_P2_PID:-}" "${SHARD1_P1_PID:-}"; do
        if [ -n "$pid" ]; then
            kill "$pid" 2>/dev/null || true
        fi
    done

    # Best-effort cleanup for dbtest/shard wrappers tied to this run's config.
    # This prevents leaked followers when the leader aborts or the script is interrupted.
    pkill -TERM -f "bash/shard.sh 2 " 2>/dev/null || true
    if [ -n "${TEMP_CONFIG:-}" ]; then
        pkill -TERM -f "$TEMP_CONFIG" 2>/dev/null || true
    else
        pkill -TERM -f "local-shards2-warehouses${trd}\\.yml.*--is-replicated" 2>/dev/null || true
    fi
    sleep 1
    pkill -9 -f "bash/shard.sh 2 " 2>/dev/null || true
    if [ -n "${TEMP_CONFIG:-}" ]; then
        pkill -9 -f "$TEMP_CONFIG" 2>/dev/null || true
    else
        pkill -9 -f "local-shards2-warehouses${trd}\\.yml.*--is-replicated" 2>/dev/null || true
    fi

    for pid in "${LEADER_PID:-}" "${SHARD0_LEARNER_PID:-}" "${SHARD0_P2_PID:-}" "${SHARD0_P1_PID:-}" \
               "${SHARD1_LEARNER_PID:-}" "${SHARD1_P2_PID:-}" "${SHARD1_P1_PID:-}"; do
        if [ -n "$pid" ]; then
            wait "$pid" 2>/dev/null || true
        fi
    done

    if [ -n "${TEMP_CONFIG:-}" ]; then
        rm -f "$TEMP_CONFIG"
    fi
    unset MAKO_CONFIG
    if [ -n "${TEMP_PAXOS_DIR:-}" ]; then
        rm -rf "$TEMP_PAXOS_DIR"
    fi
    unset MAKO_PAXOS_CONFIG_DIR
}

handle_interrupt() {
    cleanup_processes
    exit 130
}

trap cleanup_processes EXIT
trap handle_interrupt INT TERM

echo ""
echo "Configuration:"
echo "-----------------"
echo "  Number of threads: $trd"
echo "  Local shards:      0,1 (single leader process)"
echo "  Replication:       enabled (Paxos)"
echo "  Processes:         7 total (1 combined leader + 6 followers)"
echo "  Open files limit:  $current_nofile"
if [ -n "${MAKO_CPU_LIMIT:-}" ]; then
    echo "  CPU throttle:      ${MAKO_CPU_LIMIT}% (cycle=${MAKO_THROTTLE_CYCLE_MS:-default}ms)"
else
    echo "  CPU throttle:      disabled"
fi
echo "  Follower ready TO: ${follower_ready_timeout}s"
echo "  Leader startup TO: ${leader_startup_timeout}s"
echo "  Benchmark wait TO: ${max_wait}s"
echo ""

# Start follower processes for both shards first
# These need to be ready to receive replication messages
# Note: Followers use bash/shard.sh which has its own GDB support
echo "Starting follower processes..."

# Shard 0 followers
nohup bash bash/shard.sh 2 0 $trd learner 0 1 > ${log_prefix}_shard0-learner.log 2>&1 &
SHARD0_LEARNER_PID=$!
nohup bash bash/shard.sh 2 0 $trd p2 0 1 > ${log_prefix}_shard0-p2.log 2>&1 &
SHARD0_P2_PID=$!

# Shard 1 followers
nohup bash bash/shard.sh 2 1 $trd learner 0 1 > ${log_prefix}_shard1-learner.log 2>&1 &
SHARD1_LEARNER_PID=$!
nohup bash bash/shard.sh 2 1 $trd p2 0 1 > ${log_prefix}_shard1-p2.log 2>&1 &
SHARD1_P2_PID=$!

sleep 1

# Start p1 followers (after p2 and learner are up)
nohup bash bash/shard.sh 2 0 $trd p1 0 1 > ${log_prefix}_shard0-p1.log 2>&1 &
SHARD0_P1_PID=$!
nohup bash bash/shard.sh 2 1 $trd p1 0 1 > ${log_prefix}_shard1-p1.log 2>&1 &
SHARD1_P1_PID=$!

echo "Waiting for follower processes to reach ready state (timeout: ${follower_ready_timeout}s)..."
follower_ready_pattern="waiting for server communicator setup threads."
follower_ready_failed=0
wait_for_log_pattern "${log_prefix}_shard0-learner.log" "$follower_ready_pattern" "shard0 learner" "$follower_ready_timeout" || follower_ready_failed=1
wait_for_log_pattern "${log_prefix}_shard0-p2.log" "$follower_ready_pattern" "shard0 p2" "$follower_ready_timeout" || follower_ready_failed=1
wait_for_log_pattern "${log_prefix}_shard0-p1.log" "$follower_ready_pattern" "shard0 p1" "$follower_ready_timeout" || follower_ready_failed=1
wait_for_log_pattern "${log_prefix}_shard1-learner.log" "$follower_ready_pattern" "shard1 learner" "$follower_ready_timeout" || follower_ready_failed=1
wait_for_log_pattern "${log_prefix}_shard1-p2.log" "$follower_ready_pattern" "shard1 p2" "$follower_ready_timeout" || follower_ready_failed=1
wait_for_log_pattern "${log_prefix}_shard1-p1.log" "$follower_ready_pattern" "shard1 p1" "$follower_ready_timeout" || follower_ready_failed=1

if [ "$follower_ready_failed" -eq 1 ]; then
    echo "Error: one or more followers did not reach ready state"
    echo "Follower log tails:"
    tail -n 20 "${log_prefix}_shard0-learner.log" 2>/dev/null || true
    tail -n 20 "${log_prefix}_shard0-p2.log" 2>/dev/null || true
    tail -n 20 "${log_prefix}_shard0-p1.log" 2>/dev/null || true
    tail -n 20 "${log_prefix}_shard1-learner.log" 2>/dev/null || true
    tail -n 20 "${log_prefix}_shard1-p2.log" 2>/dev/null || true
    tail -n 20 "${log_prefix}_shard1-p1.log" 2>/dev/null || true
    exit 1
fi

# Start the combined leader process for both shards
# Key: -L 0,1 runs both shards in single process
# Need Paxos configs for BOTH shards
echo "Starting combined leader process for shards 0 and 1..."
log_file="${log_prefix}_leader.log"

CMD="./${BUILD_DIR:-build}/dbtest --num-threads $trd --shard-config $config_path -F ${MAKO_PAXOS_CONFIG_DIR}/paxos${trd}_shardidx0.yml -F ${MAKO_PAXOS_CONFIG_DIR}/paxos${trd}_shardidx1.yml -F config/paxos.yml -P localhost -L 0,1 --is-replicated --startup-timeout-sec $leader_startup_timeout"
THROTTLE_ARGS="$(mako_dbtest_throttle_args)" || exit 1
if [ -n "$THROTTLE_ARGS" ]; then
    CMD="$CMD$THROTTLE_ARGS"
fi

echo "Command: $CMD"
echo ""

nohup $GDB_PREFIX $CMD > "$log_file" 2>&1 &
LEADER_PID=$!

# Wait for benchmark to complete
echo "Waiting for benchmark to complete..."
wait_count=0
leader_exited_early=0
benchmark_completed=0
timed_out=0

while [ $wait_count -lt $max_wait ]; do
    if [ -f "$log_file" ] && grep -q "agg_persist_throughput" "$log_file" 2>/dev/null; then
        echo "Benchmark completed after ${wait_count}s"
        benchmark_completed=1
        sleep 2
        break
    fi

    if ! kill -0 "$LEADER_PID" 2>/dev/null; then
        # Leader may exit immediately after writing final metrics.
        # Give log output a brief moment to flush before classifying as failure.
        sleep 1

        if [ -f "$log_file" ] && grep -q "agg_persist_throughput" "$log_file" 2>/dev/null; then
            echo "Benchmark completed after ${wait_count}s (leader exited after writing results)"
            benchmark_completed=1
            sleep 1
            break
        fi

        # If benchmark started, continue with log-based validation below.
        if [ -f "$log_file" ] && grep -q "starting benchmark" "$log_file" 2>/dev/null; then
            echo "Leader exited after benchmark start; continuing with log-based checks"
            break
        fi

        echo "Leader process exited unexpectedly after ${wait_count}s"
        leader_exited_early=1
        break
    fi

    sleep 1
    wait_count=$((wait_count + 1))
    if [ $((wait_count % 10)) -eq 0 ]; then
        echo "  ... waiting (${wait_count}s elapsed)"
    fi
done

if [ $wait_count -ge $max_wait ] && [ "$benchmark_completed" -eq 0 ]; then
    echo "Warning: Benchmark did not complete within ${max_wait}s timeout"
    timed_out=1
fi

# Graceful shutdown
echo "Stopping processes (graceful)..."
cleanup_processes

echo ""
echo "========================================="
echo "Checking test results..."
echo "========================================="

failed=0

if [ "$leader_exited_early" -eq 1 ]; then
    echo "  X Combined leader exited before benchmark completion"
    failed=1
fi

if [ "$timed_out" -eq 1 ]; then
    echo "  X Benchmark timed out before throughput was observed"
    failed=1
fi

echo ""
echo "Checking $log_file (combined leader):"
echo "-----------------"

if [ ! -f "$log_file" ]; then
    echo "  X Log file not found"
    exit 1
fi

# Check for TPC-C sharding policy initialization
if grep -q "TPC-C Sharding: Initialized policy" "$log_file"; then
    echo "  OK TPC-C sharding policy initialized"
    # Show the initialization line for reference
    grep "TPC-C Sharding: Initialized policy" "$log_file" | tail -n 1 | sed 's/^/    /'
else
    echo "  X TPC-C sharding policy not initialized"
    failed=1
fi

# Check 1: Multi-shard mode initialization
if grep -q "Multi-shard mode: running 2 shards in this process" "$log_file"; then
    echo "  OK Multi-shard mode initialization detected (2 shards)"
else
    echo "  X Multi-shard mode initialization not found"
    failed=1
fi

# Check 2: Shared SiloRuntime creation
if grep -q "Created shared SiloRuntime" "$log_file"; then
    echo "  OK Shared SiloRuntime created for multi-shard mode"
else
    echo "  X Shared SiloRuntime not found"
    failed=1
fi

# Check 3: ShardContext initialization for both shards
for shard in 0 1; do
    if grep -q "Initialized ShardContext for shard $shard" "$log_file"; then
        echo "  OK ShardContext initialized for shard $shard"
    else
        echo "  X ShardContext for shard $shard not initialized"
        failed=1
    fi
done

# Check 4: Workers running for both shards
for shard in 0 1; do
    if grep -q "Running workers for shard $shard in thread" "$log_file"; then
        echo "  OK Workers running for shard $shard"
    else
        echo "  X Workers not running for shard $shard"
        failed=1
    fi
done

# Check 5: Throughput output (required for success)
if grep -q "agg_persist_throughput" "$log_file"; then
    echo "  OK Found 'agg_persist_throughput' keyword"
    grep "agg_persist_throughput" "$log_file" | tail -1 | sed 's/^/    /'
else
    echo "  X 'agg_persist_throughput' keyword not found"
    failed=1
fi

# Check 6: No transaction abort panics
if grep -q "IT should never happen" "$log_file"; then
    echo "  X Transaction abort panic detected!"
    grep "IT should never happen" "$log_file" | head -3 | sed 's/^/    /'
    failed=1
else
    echo "  OK No transaction abort panics"
fi

echo ""
echo "========================================="
if [ $failed -eq 0 ]; then
    echo "All checks passed!"
    echo "2-shard single process mode WITH replication working correctly."
    echo "========================================="
    exit 0
else
    echo "Some checks failed!"
    echo "========================================="
    echo ""
    echo "Debug information:"
    echo ""
    echo "Last 20 lines of leader log ($log_file):"
    tail -20 "$log_file"
    exit 1
fi
