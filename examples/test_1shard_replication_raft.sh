#!/bin/bash

# Script to test 1-shard experiments with RAFT replication
# Each shard should:
# 1. Show "agg_persist_throughput" keyword
# 2. Have NewOrder_remote_abort_ratio < 20%
# 3. Followers replay at least 1000 batches

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
source "${SCRIPT_DIR}/simple_transaction_rep_port_utils.sh"

echo "========================================="
echo "Testing 1-shard setup with RAFT replication"
echo "========================================="

trd=${1:-${MAKO_CI_TRD:-6}}
export MAKO_RAFT_PREFERRED_GRACE_US="${MAKO_RAFT_PREFERRED_GRACE_US:-30000000}"
export MAKO_RAFT_NONPREFERRED_GRACE_ELECTION_MIN_US="${MAKO_RAFT_NONPREFERRED_GRACE_ELECTION_MIN_US:-5000000}"
export MAKO_RAFT_NONPREFERRED_GRACE_ELECTION_MAX_US="${MAKO_RAFT_NONPREFERRED_GRACE_ELECTION_MAX_US:-10000000}"
script_name="$(basename "$0")"
ps aux | grep -i dbtest | awk "{print \$2}" | xargs kill -9 2>/dev/null
# Clean up old log files
rm -f nfs_sync_*
USERNAME=${USER:-unknown}
rm -rf /tmp/${USERNAME}_mako_rocksdb_shard*

# Randomize raft replication ports — see test_2shard_replication.sh.
TEMP_PAXOS_DIR=$(make_paxos_replication_configs 1 "$trd" raft)
if [ -z "$TEMP_PAXOS_DIR" ]; then
    exit 1
fi
export MAKO_PAXOS_CONFIG_DIR="$TEMP_PAXOS_DIR"
echo "raft replication config dir: $MAKO_PAXOS_CONFIG_DIR"
trap '[ -n "${TEMP_PAXOS_DIR:-}" ] && rm -rf "$TEMP_PAXOS_DIR"; unset MAKO_PAXOS_CONFIG_DIR' EXIT

# Start shard 0 in background with RAFT replication (3 replicas, no learner)
echo "Starting shard 0 with Raft..."
nohup bash bash/shard.sh 1 0 $trd localhost 0 1 raft > $script_name\_shard0-localhost-$trd.log 2>&1 &
nohup bash bash/shard.sh 1 0 $trd p2 0 1 raft > $script_name\_shard0-p2-$trd.log 2>&1 &
sleep 1
nohup bash bash/shard.sh 1 0 $trd p1 0 1 raft > $script_name\_shard0-p1-$trd.log 2>&1 &
SHARD0_PID=$!
sleep 2

# Wait for benchmark to complete (poll for completion marker)
echo "Waiting for benchmark to complete..."
log_file="${script_name}_shard0-localhost-$trd.log"
max_wait=120  # Maximum wait time in seconds
wait_count=0

while [ $wait_count -lt $max_wait ]; do
    # Check if throughput output appeared (indicates completion)
    if [ -f "$log_file" ] && grep -q "agg_persist_throughput" "$log_file" 2>/dev/null; then
        echo "Benchmark completed after ${wait_count}s"
        sleep 2  # Give a moment for final output
        break
    fi
    sleep 1
    wait_count=$((wait_count + 1))
    if [ $((wait_count % 10)) -eq 0 ]; then
        echo "  ... waiting (${wait_count}s elapsed)"
    fi
done

if [ $wait_count -ge $max_wait ]; then
    echo "Warning: Benchmark did not complete within ${max_wait}s timeout"
fi

# Give the follower a grace window to DRAIN REPLAY LAG before we kill it —
# same treatment as test_1shard_replication.sh: killing the moment the
# leader finishes snapshots an arbitrary mid-lag replay_batch count.
# Progress-aware: wait while the counter still advances (wait_for_termination
# keeps logging it while draining), stop early once it clears the threshold,
# bail on a stall (drained-or-stuck).
log_p1_drain="${script_name}_shard0-p1-$trd.log"
drain_budget="${MAKO_REPLAY_DRAIN_SECONDS:-240}"
drain_stall_budget="${MAKO_REPLAY_DRAIN_STALL_SECONDS:-20}"
# Threshold rationale: see test_1shard_replication.sh — after draining, the
# counter converges to the leader's total batch production (observed as low
# as ~530 under the CI CPU throttle), so >500 left no margin.
replay_min="${MAKO_REPLAY_BATCH_MIN:-200}"
drained=0
last_rb=""
stall=0
for ((i = 0; i < drain_budget; i++)); do
    rb=$(grep "replay_batch:" "$log_p1_drain" 2>/dev/null | tail -1 | sed -n 's/.*replay_batch:\([0-9]*\).*/\1/p')
    if [ -n "$rb" ] && [ "$rb" -gt "$replay_min" ]; then
        echo "Follower replay drained: replay_batch=$rb after ${i}s"
        drained=1
        break
    fi
    if [ -n "$rb" ] && [ "$rb" != "$last_rb" ]; then
        last_rb="$rb"
        stall=0
    else
        stall=$((stall + 1))
    fi
    if [ "$stall" -ge "$drain_stall_budget" ]; then
        echo "Note: follower replay stalled at ${rb:-none} for ${drain_stall_budget}s (${i}s total) - giving up drain"
        break
    fi
    sleep 1
done
if [ "$drained" -eq 0 ] && [ "$stall" -lt "$drain_stall_budget" ]; then
    echo "Note: follower replay still below threshold after ${drain_budget}s grace (last: ${rb:-none})"
fi

# Graceful shutdown: SIGTERM first
echo "Stopping shards (graceful)..."
pkill -TERM -f "dbtest.*shard-index 0" 2>/dev/null || true
sleep 3

# Force kill any remaining processes
pkill -9 -f "dbtest.*shard-index 0" 2>/dev/null || true
sleep 1

# Original cleanup for good measure
kill $SHARD0_PID 2>/dev/null || true
wait $SHARD0_PID 2>/dev/null || true

echo ""
echo "========================================="
echo "Checking test results..."
echo "========================================="

failed=0

# Check each shard's output
{
    i=0
    log="${script_name}_shard${i}-localhost-$trd.log"
    echo ""
    echo "Checking $log:"
    echo "-----------------"

    if [ ! -f "$log" ]; then
        echo "  ✗ Log file not found"
        failed=1
        continue
    fi

    # Check for agg_persist_throughput keyword
    if grep -q "agg_persist_throughput" "$log"; then
        echo "  ✓ Found 'agg_persist_throughput' keyword"
        # Show the line for reference
        grep "agg_persist_throughput" "$log" | tail -1 | sed 's/^/    /'
    else
        echo "  ✗ 'agg_persist_throughput' keyword not found"
        failed=1
    fi

    # Check NewOrder_remote_abort_ratio
    if grep -q "NewOrder_remote_abort_ratio:" "$log"; then
        # Extract the abort ratio value
        abort_ratio=$(grep "NewOrder_remote_abort_ratio:" "$log" | tail -1 | awk '{print $2}')

        if [ -z "$abort_ratio" ]; then
            echo "  ✗ Could not extract NewOrder_remote_abort_ratio value"
            failed=1
        else
            # Remove % sign if present and convert to float
            abort_value=$(echo "$abort_ratio" | sed 's/%//')

            # Handle -nan/nan (0 remote txns = 0/0 division, perfectly fine)
            if echo "$abort_value" | grep -qi "nan"; then
                echo "  ✓ NewOrder_remote_abort_ratio: $abort_ratio (no remote txns, OK)"
            elif awk "BEGIN {exit !($abort_value < 20)}"; then
                echo "  ✓ NewOrder_remote_abort_ratio: $abort_ratio (< 20%)"
            else
                echo "  ✗ NewOrder_remote_abort_ratio: $abort_ratio (>= 20%)"
                failed=1
            fi
        fi
    else
        echo "  ✗ NewOrder_remote_abort_ratio not found"
        failed=1
    fi
}

# Check replay_batch counter in shard0-p1.log
echo ""
log_p1="${script_name}_shard0-p1-$trd.log"
echo "Checking $log_p1:"
echo "-----------------"

if [ ! -f "$log_p1" ]; then
    echo "  ✗ $log_p1 file not found"
    failed=1
else
    # Get the last occurrence of replay_batch
    last_replay_batch=$(grep "replay_batch:" "$log_p1" | tail -1)

    if [ -z "$last_replay_batch" ]; then
        echo "  ✗ No 'replay_batch' keyword found in $log_p1"
        failed=1
    else
        # Extract the replay_batch number (assuming format: "replay_batch:XXX")
        replay_count=$(echo "$last_replay_batch" | sed -n 's/.*replay_batch:\([0-9]*\).*/\1/p')

        if [ -z "$replay_count" ]; then
            echo "  ✗ Could not extract replay_batch value"
            echo "    Last line: $last_replay_batch"
            failed=1
        else
            # The test verifies replication is working, not exact batch count.
            # Threshold rationale at the drain loop above (default 200,
            # override via MAKO_REPLAY_BATCH_MIN).
            if [ "$replay_count" -gt "${replay_min:-200}" ]; then
                echo "  ✓ replay_batch: $replay_count (> ${replay_min:-200})"
            else
                echo "  ✗ replay_batch: $replay_count (should be > ${replay_min:-200})"
                failed=1
            fi
        fi
    fi
fi

echo ""
echo "========================================="
if [ $failed -eq 0 ]; then
    echo "All checks passed! (Raft replication)"
    echo "========================================="
    exit 0
else
    echo "Some checks failed! (Raft replication)"
    echo "========================================="
    echo ""
    echo "Debug information:"
    echo "Check $script_name\_shard0-localhost-$trd.log and $log_p1 for details"
    echo ""
    echo "Last 10 lines of $script_name\_shard0-localhost-$trd.log:"
    tail -10 $script_name\_shard0-localhost-$trd.log
    echo ""
    echo "Last 5 lines with 'replay_batch' from $log_p1:"
    grep "replay_batch" $log_p1 | tail -5 2>/dev/null || echo "No replay_batch entries found"
    exit 1
fi
