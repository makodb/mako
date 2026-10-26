#!/bin/bash

# Failover test for Mako's cross-shard failover under Raft. Runs 2 shards x 3
# replicas (localhost, p1, p2), takes one shard's leader away while the
# benchmark runs, and checks from the logs that:
#   1. one of that shard's followers wins the election and takes over: it
#      pauses the other shard, closes the old epoch, takes the stable cut from
#      both shards' final watermarks and starts its workers in the new epoch,
#   2. the other shard pauses, moves to the new epoch, reads the failed
#      shard's final watermark from the new leader, and takes the same cut,
#   3. the new leader and the other shard both finish their runs and report
#      throughput, the other shard with its remote abort ratio below 40%,
#   4. no surviving process crashed.
# With MAKO_FAILOVER_EVENT=twice the leader is paused instead of killed, and
# once a follower has taken over, the old leader resumes and that follower is
# paused at once. The old leader's log is behind, so the other follower wins
# and takes over a second time; the test then checks 1-4 for the second
# takeover, which must read the other shard's final watermark of the second
# epoch, not the first's. With MAKO_FAILOVER_EVENT=back the old leader gets a
# few seconds to catch up before that follower is paused, so it wins the
# second election itself and takes the shard back.
#
# Usage: test_2shard_failover_raft.sh [threads]
# Env: MAKO_FAILOVER_SHARD (the shard whose leader goes, 0 or 1, default 0),
#      MAKO_FAILOVER_EVENT (kill, twice or back, default kill),
#      MAKO_FAILOVER_AFTER (seconds into the run before the event, default 8),
#      MAKO_FAILOVER_TIMEOUT (seconds after the event for both runs to end,
#      default 240).
# Exit status: 0 passed, 1 a check failed, 3 the cluster never got as far as
# the test, which is the only case worth retrying.

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
source "${SCRIPT_DIR}/simple_transaction_rep_port_utils.sh"

trd=${1:-${MAKO_CI_TRD:-6}}
F="${MAKO_FAILOVER_SHARD:-0}"   # the failed shard
case "$F" in
    0) O=1 ;;
    1) O=0 ;;
    *) echo "MAKO_FAILOVER_SHARD must be 0 or 1"; exit 2 ;;
esac
event="${MAKO_FAILOVER_EVENT:-kill}"
case "$event" in
    kill|twice|back) ;;
    *) echo "MAKO_FAILOVER_EVENT must be kill, twice or back"; exit 2 ;;
esac
fail_after="${MAKO_FAILOVER_AFTER:-8}"
fail_timeout="${MAKO_FAILOVER_TIMEOUT:-240}"

echo "========================================="
echo "Testing 2-shard failover with RAFT replication (shard $F's leader, $event)"
echo "========================================="

rm -f nfs_sync_*
USERNAME=${USER:-unknown}
rm -rf /tmp/${USERNAME}_mako_rocksdb_shard*

export MAKO_RAFT_PREFERRED_GRACE_US="${MAKO_RAFT_PREFERRED_GRACE_US:-30000000}"
export MAKO_RAFT_NONPREFERRED_GRACE_ELECTION_MIN_US="${MAKO_RAFT_NONPREFERRED_GRACE_ELECTION_MIN_US:-5000000}"
export MAKO_RAFT_NONPREFERRED_GRACE_ELECTION_MAX_US="${MAKO_RAFT_NONPREFERRED_GRACE_ELECTION_MAX_US:-10000000}"
# The new leader's run ends later than a normal one; followers wait for it.
export MAKO_FOLLOWER_END_WAIT_SECONDS="${MAKO_FOLLOWER_END_WAIT_SECONDS:-600}"
script_name="$(basename "$0")"
binary_path="./${BUILD_DIR:-build}/dbtest"
log_prefix="${script_name}_srpc_${event}${F}"
PIDS=()
STOPPED=()
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
    # A stopped process acts on SIGTERM only once it runs again.
    for pid in "${STOPPED[@]}"; do
        kill -CONT "$pid" 2>/dev/null || true
    done
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
pid_of() {  # pid_of <shard> <role>
    pgrep -f -- "--shard-index $1 --shard-config ${MAKO_CONFIG} -P $2( |\$)" | head -1
}
# The epoch a replica of shard F last took over with ("" if it never did).
takeover_epoch() {
    grep -a -o "\[RAFT-TAKEOVER\] shard $F: epoch [0-9]*" "$(log $F-$1)" 2>/dev/null | tail -1 | sed 's/.* //'
}
leaderF="$(log $F-localhost)"

# Wait for the failing shard's leader to start its workers.
waited=0
while ! grep -q "ADVANCER-SEND" "$leaderF" 2>/dev/null; do
    if [ "$waited" -ge 180 ]; then
        echo "  ✗ shard $F's leader never started its workers"
        tail -20 "$leaderF"
        exit 3
    fi
    sleep 1
    waited=$((waited + 1))
done
echo "Shard $F's leader is running; taking it away in ${fail_after}s"
sleep "$fail_after"

victim=$(pid_of "$F" localhost)
if [ -z "$victim" ]; then
    echo "  ✗ shard $F's leader is not running"
    exit 3
fi
if grep -q "agg_persist_throughput" "$leaderF"; then
    echo "  ✗ shard $F's leader finished its run before the event"
    exit 3
fi
if [ "$event" = kill ]; then
    kill -9 "$victim"
    echo "Killed shard $F's leader (pid $victim)"
else
    kill -STOP "$victim"
    STOPPED+=("$victim")
    echo "Paused shard $F's leader (pid $victim)"
fi

# Wait until a replica of shard F other than $1 takes over with an epoch
# above first_epoch; sets new_leader and new_epoch.
wait_for_takeover() {
    local skip=$1 r e
    new_leader=""
    new_epoch=""
    while [ "$waited" -lt "$fail_timeout" ]; do
        for r in localhost p1 p2; do
            [ "$r" = "$skip" ] && continue
            e=$(takeover_epoch "$r")
            if [ -n "$e" ] && { [ -z "$first_epoch" ] || [ "$e" -gt "$first_epoch" ]; }; then
                new_leader=$r
                new_epoch=$e
                echo "  shard $F's $r took over (epoch $e) ${waited}s after the event"
                return 0
            fi
        done
        sleep 1
        waited=$((waited + 1))
    done
    return 1
}

waited=0
first_epoch=""
first_leader=""
if [ "$event" != kill ]; then
    if ! wait_for_takeover localhost; then
        echo "  ✗ no replica of shard $F took over the first time"
        exit 1
    fi
    first_leader=$new_leader
    first_epoch=$new_epoch
    second_victim=$(pid_of "$F" "$first_leader")
    kill -CONT "$victim"
    if [ "$event" = back ]; then
        # The old leader catches up first, so it can win the next election.
        sleep 5
    fi
    kill -STOP "$second_victim"
    STOPPED+=("$second_victim")
    echo "Resumed shard $F's old leader and paused $first_leader (pid $second_victim)"
    if ! wait_for_takeover "$first_leader"; then
        echo "  ✗ no other replica of shard $F took over the second time"
        exit 1
    fi
    if [ "$event" = back ] && [ "$new_leader" != localhost ]; then
        echo "  ✗ $new_leader won the second election, not the old leader; the case did not run"
        exit 3
    fi
else
    wait_for_takeover localhost
fi

# Wait for the new leader and the other shard to finish their runs.
done_both=0
while [ -n "$new_leader" ] && [ "$waited" -lt "$fail_timeout" ]; do
    if grep -q "agg_persist_throughput" "$(log $F-$new_leader)" 2>/dev/null &&
       grep -q "agg_persist_throughput" "$(log $O-localhost)" 2>/dev/null; then
        done_both=1
        sleep 2
        break
    fi
    sleep 1
    waited=$((waited + 1))
    if [ $((waited % 15)) -eq 0 ]; then
        echo "  ... waiting (${waited}s after the event, new leader: ${new_leader})"
    fi
done

# Any survivor that exited before the end is checked below by its log.
cleanup_processes

echo ""
echo "========================================="
echo "Checking test results..."
echo "========================================="
failed=0

# The cut a process took last: its last update_stable_timestamp_vec line.
cut_of() {
    grep -a -o "update_stable_timestamp_vec, epoch:[0-9]*,tt:[0-9]*" "$1" | tail -1 | sed 's/.*tt://'
}
# The final watermark of shard $2 that a process of the failed shard read
# last. Only its "get fvw" lines name the other shard's key ("set fvw" names
# its own), and another thread's output can land inside the line, before the
# key, so match from the key on.
fvw_read() {
    grep -a -o "fvw_$2:[0-9]*" "$1" | tail -1 | sed 's/.*://'
}
# A process's threads share its log, and another thread's output can land
# right after a number, so one of two equal numbers may carry extra digits.
same_number() {
    [ -n "$1" ] && [ -n "$2" ] && { [[ "$1" == "$2"* ]] || [[ "$2" == "$1"* ]]; }
}

so="$(log $O-localhost)"
if [ -z "$new_leader" ]; then
    echo "  ✗ no shard $F replica took over"
    grep -h "RAFT-TAKEOVER" "$(log $F-p1)" "$(log $F-p2)" | sed 's/^/    /'
    failed=1
else
    nl="$(log $F-$new_leader)"
    if [ "$event" != kill ]; then
        echo "  ✓ shard $F's $first_leader took over (epoch $first_epoch), then $new_leader (epoch $new_epoch):"
    else
        echo "  ✓ shard $F's $new_leader took over:"
    fi
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
    if [ "$event" != kill ]; then
        w1=$(fvw_read "$(log $F-$first_leader)" "$O")
        w2=$(fvw_read "$nl" "$O")
        if [ -n "$w1" ] && [ -n "$w2" ] && ! same_number "$w1" "$w2"; then
            echo "  ✓ the second takeover read shard $O's new final watermark ($w2, the first read $w1)"
        else
            echo "  ✗ the second takeover read shard $O's final watermark '$w2', the first '$w1'"
            failed=1
        fi
    fi
fi

controls=1
[ "$event" != kill ] && controls=2
if [ "$(grep -c "register_fasttransport_for_bench: 0" "$so")" -ge "$controls" ] &&
   [ "$(grep -c "register_fasttransport_for_bench: 1" "$so")" -ge "$controls" ]; then
    echo "  ✓ shard $O paused and moved to the new epoch (${controls}x)"
    grep -h "update_stable_timestamp_vec\|get fvw" "$so" | sed 's/^/    /'
else
    echo "  ✗ shard $O did not get both failover controls ${controls}x"
    failed=1
fi
if [ -n "$new_leader" ]; then
    cut_new=$(cut_of "$nl")
    cut_other=$(cut_of "$so")
    if same_number "$cut_new" "$cut_other"; then
        echo "  ✓ both shards cut the old epoch at the same point ($cut_new)"
    else
        echo "  ✗ the shards cut the old epoch at different points: shard $F at '${cut_new}', shard $O at '${cut_other}'"
        failed=1
    fi
fi
if [ "$done_both" -eq 1 ]; then
    echo "  ✓ shard $O finished: $(grep "agg_persist_throughput" "$so" | tail -1)"
    abort_ratio=$(grep "NewOrder_remote_abort_ratio:" "$so" | tail -1 | awk '{print $2}' | sed 's/%//')
    if [ -n "$abort_ratio" ] && awk "BEGIN {exit !($abort_ratio < 40)}"; then
        echo "  ✓ shard $O's NewOrder_remote_abort_ratio: ${abort_ratio}% (< 40%)"
    else
        echo "  ✗ shard $O's NewOrder_remote_abort_ratio: ${abort_ratio:-missing}"
        failed=1
    fi
else
    echo "  ✗ the runs did not both finish within ${fail_timeout}s of the event"
    failed=1
fi

for f in "$(log $F-localhost)" "$(log $F-p1)" "$(log $F-p2)" "$so" "$(log $O-p1)" "$(log $O-p2)"; do
    [ "$event" = kill ] && [ "$f" = "$leaderF" ] && continue
    if grep -q "Segmentation fault\|core dumped\|terminate called\|PANIC" "$f"; then
        echo "  ✗ $f shows a crash:"
        grep -m3 "Segmentation fault\|core dumped\|terminate called\|PANIC" "$f" | sed 's/^/    /'
        failed=1
    fi
done

echo ""
echo "========================================="
if [ $failed -eq 0 ]; then
    echo "All checks passed! (Raft failover, shard $F, $event)"
    echo "========================================="
    exit 0
fi
echo "Some checks failed! (Raft failover, shard $F, $event)"
echo "========================================="
# What the runs did: shard $O's failover controls, and per transaction type
# its remote share, abort ratios and latencies (an abort latency near the
# RPC timeout, 250 ms, means requests got no answer); the failed shard's
# elections and takeovers.
echo "--- shard $O ($so)"
grep -a "register_fasttransport_for_bench" "$so" | sed 's/^/    /'
grep -a "_remote_ratio:\|_abort_ratio:\|_abort_latency:\|_commit_latency:" "$so" | tail -20 | sed 's/^/    /'
for f in "$(log $F-localhost)" "$(log $F-p1)" "$(log $F-p2)"; do
    echo "--- $f"
    grep -a "became leader\|RAFT-TAKEOVER\|client for the control is starting\|_abort_ratio:" "$f" | tail -12 | sed 's/^/    /'
done
exit 1
