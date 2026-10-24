#!/bin/bash

# Failover test for Mako's cross-shard failover under Raft. Runs 2 shards x 3
# replicas (localhost, p1, p2), kills shard 0's leader while the benchmark
# runs, and checks from the logs that:
#   1. one of shard 0's followers wins the election and takes over: it pauses
#      shard 1, closes the old epoch, takes the stable cut from both shards'
#      final watermarks and starts its workers in the new epoch,
#   2. shard 1 pauses, moves to the new epoch and reads shard 0's final
#      watermark from the new leader,
#   3. the new leader and shard 1 both finish their runs and report
#      throughput, shard 1 with its remote abort ratio below 40%,
#   4. no surviving process crashed.
#
# Usage: test_2shard_failover_raft.sh [threads]
# Env: MAKO_FAILOVER_AFTER (seconds into the run before the kill, default 8),
#      MAKO_FAILOVER_TIMEOUT (seconds after the kill for both runs to end,
#      default 240).

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
source "${SCRIPT_DIR}/simple_transaction_rep_port_utils.sh"

echo "========================================="
echo "Testing 2-shard failover with RAFT replication"
echo "========================================="

rm -f nfs_sync_*
USERNAME=${USER:-unknown}
rm -rf /tmp/${USERNAME}_mako_rocksdb_shard*

trd=${1:-${MAKO_CI_TRD:-6}}
fail_after="${MAKO_FAILOVER_AFTER:-8}"
fail_timeout="${MAKO_FAILOVER_TIMEOUT:-240}"
export MAKO_RAFT_PREFERRED_GRACE_US="${MAKO_RAFT_PREFERRED_GRACE_US:-30000000}"
export MAKO_RAFT_NONPREFERRED_GRACE_ELECTION_MIN_US="${MAKO_RAFT_NONPREFERRED_GRACE_ELECTION_MIN_US:-5000000}"
export MAKO_RAFT_NONPREFERRED_GRACE_ELECTION_MAX_US="${MAKO_RAFT_NONPREFERRED_GRACE_ELECTION_MAX_US:-10000000}"
# The new leader's run ends later than a normal one; followers wait for it.
export MAKO_FOLLOWER_END_WAIT_SECONDS="${MAKO_FOLLOWER_END_WAIT_SECONDS:-600}"
script_name="$(basename "$0")"
binary_path="./${BUILD_DIR:-build}/dbtest"
log_prefix="${script_name}_srpc"
PIDS=()
CLEANUP_DONE=0

if [ ! -x "$binary_path" ]; then
    echo "Error: dbtest binary not found or not executable at '$binary_path'"
    exit 1
fi

TEMP_CONFIG=$(make_simple_txn_rep_config 2 $trd)
if [ -z "$TEMP_CONFIG" ]; then
    exit 1
fi
export MAKO_CONFIG="$TEMP_CONFIG"
echo "dbtest config: $MAKO_CONFIG"

TEMP_PAXOS_DIR=$(make_paxos_replication_configs 2 "$trd" raft)
if [ -z "$TEMP_PAXOS_DIR" ]; then
    exit 1
fi
export MAKO_PAXOS_CONFIG_DIR="$TEMP_PAXOS_DIR"
echo "raft replication config dir: $MAKO_PAXOS_CONFIG_DIR"

cleanup_processes() {
    if [ "$CLEANUP_DONE" -eq 1 ]; then
        return
    fi
    CLEANUP_DONE=1
    for pid in "${PIDS[@]}"; do
        kill "$pid" 2>/dev/null || true
    done
    pkill -TERM -f "$TEMP_CONFIG" 2>/dev/null || true
    sleep 1
    pkill -9 -f "$TEMP_CONFIG" 2>/dev/null || true
    for pid in "${PIDS[@]}"; do
        wait "$pid" 2>/dev/null || true
    done
    rm -f "$TEMP_CONFIG"
    unset MAKO_CONFIG
    rm -rf "$TEMP_PAXOS_DIR"
    unset MAKO_PAXOS_CONFIG_DIR
}

handle_interrupt() {
    cleanup_processes
    exit 130
}

trap cleanup_processes EXIT
trap handle_interrupt INT TERM

pkill -9 -x dbtest 2>/dev/null || true
sleep 1

start_replica() {
    local shard=$1 role=$2
    nohup bash bash/shard.sh 2 "$shard" "$trd" "$role" 0 1 raft > "${log_prefix}_shard${shard}-${role}.log" 2>&1 &
    PIDS+=($!)
}

echo "Starting shard 0 with Raft..."
start_replica 0 localhost
start_replica 0 p2
sleep 1
start_replica 0 p1
sleep 5
echo "Starting shard 1 with Raft..."
start_replica 1 localhost
start_replica 1 p2
sleep 1
start_replica 1 p1

log() { echo "${log_prefix}_shard$1.log"; }
leader0="$(log 0-localhost)"

# Wait for shard 0's leader to start its workers.
waited=0
while ! grep -q "ADVANCER-SEND" "$leader0" 2>/dev/null; do
    if [ "$waited" -ge 180 ]; then
        echo "  ✗ shard 0's leader never started its workers"
        tail -20 "$leader0"
        exit 3
    fi
    sleep 1
    waited=$((waited + 1))
done
echo "Shard 0's leader is running; killing it in ${fail_after}s"
sleep "$fail_after"

victim=$(pgrep -f -- "--shard-index 0 --shard-config ${MAKO_CONFIG} -P localhost" | head -1)
if [ -z "$victim" ]; then
    echo "  ✗ shard 0's leader is not running"
    exit 3
fi
if grep -q "agg_persist_throughput" "$leader0"; then
    echo "  ✗ shard 0's leader finished its run before the kill"
    exit 3
fi
kill -9 "$victim"
echo "Killed shard 0's leader (pid $victim)"

# Wait for the new leader and shard 1 to finish their runs.
new_leader=""
waited=0
done_both=0
while [ "$waited" -lt "$fail_timeout" ]; do
    if [ -z "$new_leader" ]; then
        for r in p1 p2; do
            if grep -q "\[RAFT-TAKEOVER\] shard 0: epoch" "$(log 0-$r)" 2>/dev/null; then
                new_leader=$r
                echo "  shard 0's $r took over after ${waited}s"
            fi
        done
    fi
    if [ -n "$new_leader" ] &&
       grep -q "agg_persist_throughput" "$(log 0-$new_leader)" 2>/dev/null &&
       grep -q "agg_persist_throughput" "$(log 1-localhost)" 2>/dev/null; then
        done_both=1
        sleep 2
        break
    fi
    sleep 1
    waited=$((waited + 1))
    if [ $((waited % 15)) -eq 0 ]; then
        echo "  ... waiting (${waited}s after the kill, new leader: ${new_leader:-none})"
    fi
done

# Any survivor that exited before the end is checked below by its log.
cleanup_processes

echo ""
echo "========================================="
echo "Checking test results..."
echo "========================================="
failed=0

if [ -z "$new_leader" ]; then
    echo "  ✗ no shard 0 replica took over"
    grep -h "RAFT-TAKEOVER" "$(log 0-p1)" "$(log 0-p2)" | sed 's/^/    /'
    failed=1
else
    nl="$(log 0-$new_leader)"
    echo "  ✓ shard 0's $new_leader took over:"
    grep -h "RAFT-TAKEOVER\|get fvw" "$nl" | sed 's/^/    /'
    if [ "$(grep -c "get fvw" "$nl")" -ge 2 ]; then
        echo "  ✓ it took the stable cut from both shards' final watermarks"
    else
        echo "  ✗ it did not read both shards' final watermarks"
        failed=1
    fi
    if grep -q "agg_persist_throughput" "$nl"; then
        echo "  ✓ its workers ran: $(grep "agg_persist_throughput" "$nl" | tail -1)"
    else
        echo "  ✗ its workers did not finish a run"
        failed=1
    fi
fi

s1="$(log 1-localhost)"
if grep -q "register_fasttransport_for_bench: 0" "$s1" &&
   grep -q "register_fasttransport_for_bench: 1" "$s1"; then
    echo "  ✓ shard 1 paused and moved to the new epoch"
    grep -h "update_stable_timestamp_vec\|get fvw" "$s1" | sed 's/^/    /'
else
    echo "  ✗ shard 1 did not get both failover controls"
    failed=1
fi
if [ "$done_both" -eq 1 ]; then
    echo "  ✓ shard 1 finished: $(grep "agg_persist_throughput" "$s1" | tail -1)"
    abort_ratio=$(grep "NewOrder_remote_abort_ratio:" "$s1" | tail -1 | awk '{print $2}' | sed 's/%//')
    if [ -n "$abort_ratio" ] && awk "BEGIN {exit !($abort_ratio < 40)}"; then
        echo "  ✓ shard 1's NewOrder_remote_abort_ratio: ${abort_ratio}% (< 40%)"
    else
        echo "  ✗ shard 1's NewOrder_remote_abort_ratio: ${abort_ratio:-missing}"
        failed=1
    fi
else
    echo "  ✗ the runs did not both finish within ${fail_timeout}s of the kill"
    failed=1
fi

for f in "$(log 0-p1)" "$(log 0-p2)" "$s1" "$(log 1-p1)" "$(log 1-p2)"; do
    if grep -q "Segmentation fault\|core dumped\|terminate called\|PANIC" "$f"; then
        echo "  ✗ $f shows a crash:"
        grep -m3 "Segmentation fault\|core dumped\|terminate called\|PANIC" "$f" | sed 's/^/    /'
        failed=1
    fi
done

echo ""
echo "========================================="
if [ $failed -eq 0 ]; then
    echo "All checks passed! (Raft failover)"
    echo "========================================="
    exit 0
fi
echo "Some checks failed! (Raft failover)"
echo "========================================="
for f in "$(log 0-p1)" "$(log 0-p2)" "$s1"; do
    echo "--- $f"
    tail -10 "$f"
done
exit 1
