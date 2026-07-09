#!/bin/bash

# Live operator-driven range migration on the REAL multi-process bed --
# PARTITION-focused: two shard processes, NO replication (replication is
# orthogonal to what this validates and the replicated variant just adds six
# processes of startup surface; layer it on later via shard2Replication +
# MAKO_CLUSTER_CONFIG=1 when replication x migration interaction matters).
#
# Same 2-shard topology as test_2shard_no_replication.sh, plus the cluster
# runtime enabled (MAKO_CLUSTER_CONFIG=1, map routing). Shard 1 seeds 30 demo
# rows into the migratable index (__mako_kv__); once both shards' data planes
# are up, mako_admin fires ONE Migrate RPC at shard 0's MigrationAdmin service,
# which drives the STANDING shard-0 ShardMaster through the full online 2PC
# against the real data planes -- the destination local, the source remote over
# the ShardDataService rrr socket -- WHILE TPC-C serves.
#
# Pass criteria:
#   1. mako_admin exits 0 with "ok=1 moved=10" (migration committed, 10 rows).
#   2. Shard 0's log shows the MigrationAdmin committed line.
#   3. The usual bed checks still pass (throughput + abort ratio on both shards).

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
source "${SCRIPT_DIR}/simple_transaction_rep_port_utils.sh"

echo "========================================="
echo "Testing 2-shard live admin-driven migration (partition bed, no replication)"
echo "========================================="

rm -f nfs_sync_*
USERNAME=${USER:-$(whoami)}
rm -rf /tmp/${USERNAME}_mako_rocksdb_shard*

trd=${1:-${MAKO_CI_TRD:-4}}
script_name="$(basename "$0")"
binary_path="./${BUILD_DIR:-build}/dbtest"
admin_path="./${BUILD_DIR:-build}/mako_admin"
SHARD0_PID=""
SHARD1_PID=""
CLEANUP_DONE=0

if [ ! -x "$binary_path" ]; then
    echo "Error: dbtest binary not found or not executable at '$binary_path'"
    exit 1
fi
if [ ! -x "$admin_path" ]; then
    echo "Error: mako_admin binary not found or not executable at '$admin_path'"
    exit 1
fi

# Randomized port base to avoid collisions on shared hosts.
TEMP_CONFIG=$(make_simple_txn_rep_config 2 $trd)
if [ -z "$TEMP_CONFIG" ]; then
    exit 1
fi
export MAKO_CONFIG="$TEMP_CONFIG"
echo "dbtest config: $MAKO_CONFIG"

# The cluster runtime under test: config bootstrap + map routing + the shard
# data planes; shard 1 seeds the migratable rows the admin will move.
export MAKO_CLUSTER_CONFIG=1
export MAKO_SHARDING_MODE=map
export MAKO_MIGRATE_SEED=30
export MAKO_MIGRATE_SEED_SHARD=1

cleanup_temp_config() {
    if [ "$CLEANUP_DONE" -eq 1 ]; then
        return
    fi
    CLEANUP_DONE=1
    for pid in "${SHARD0_PID:-}" "${SHARD1_PID:-}"; do
        if [ -n "$pid" ]; then
            kill "$pid" 2>/dev/null || true
        fi
    done
    if [ -n "${TEMP_CONFIG:-}" ]; then
        pkill -TERM -f "$TEMP_CONFIG" 2>/dev/null || true
        sleep 1
        pkill -9 -f "$TEMP_CONFIG" 2>/dev/null || true
    fi
    for pid in "${SHARD0_PID:-}" "${SHARD1_PID:-}"; do
        if [ -n "$pid" ]; then
            wait "$pid" 2>/dev/null || true
        fi
    done
    rm -f "$TEMP_CONFIG"
    unset MAKO_CONFIG
}

handle_interrupt() {
    cleanup_temp_config
    exit 130
}

trap cleanup_temp_config EXIT
trap handle_interrupt INT TERM

transport="${MAKO_TRANSPORT:-rrr}"
log_prefix="${script_name}_${transport}"
log_file0="${log_prefix}_shard0-$trd.log"
log_file1="${log_prefix}_shard1-$trd.log"

pkill -9 -x dbtest 2>/dev/null || true
sleep 1

echo "Starting shard 0..."
nohup bash bash/shard.sh 2 0 $trd localhost > "$log_file0" 2>&1 &
SHARD0_PID=$!
sleep 5

echo "Starting shard 1..."
nohup bash bash/shard.sh 2 1 $trd localhost > "$log_file1" 2>&1 &
SHARD1_PID=$!

# ---- Wait for BOTH shard data planes, then fire the migration ----
echo "Waiting for both shard data planes..."
admin_addr=""
dp_wait=0
while [ "$dp_wait" -lt 90 ]; do
    l0=$(grep -a "data plane listening on" "$log_file0" 2>/dev/null | tail -n 1)
    l1=$(grep -a "data plane listening on" "$log_file1" 2>/dev/null | tail -n 1)
    if [ -n "$l0" ] && [ -n "$l1" ]; then
        port=$(echo "$l0" | sed -n 's/.*listening on 0\.0\.0\.0:\([0-9]*\).*/\1/p')
        if [ -n "$port" ]; then
            admin_addr="127.0.0.1:${port}"
            break
        fi
    fi
    sleep 1
    dp_wait=$((dp_wait + 1))
done

migration_ok=0
admin_output=""
if [ -z "$admin_addr" ]; then
    echo "  ✗ Data planes did not come up within 90s"
else
    echo "Both data planes up; MigrationAdmin at ${admin_addr}"
    echo "Firing: mako_admin migrate ${admin_addr} __mako_kv__ d10 d20 1 -> 0 (mid-workload)"
    admin_output=$("$admin_path" migrate "$admin_addr" __mako_kv__ d10 d20 1 0 30 2>&1)
    admin_rc=$?
    echo "mako_admin: rc=${admin_rc} output: ${admin_output}"
    if [ "$admin_rc" -eq 0 ] && echo "$admin_output" | grep -q "ok=1 moved=10"; then
        migration_ok=1
    fi
fi

# ---- Wait for benchmark completion (standard bed behavior) ----
max_wait="${MAKO_MAX_WAIT_SECONDS:-120}"
if ! [[ "$max_wait" =~ ^[0-9]+$ ]] || [ "$max_wait" -le 0 ]; then
    max_wait=120
fi
wait_count=0
benchmark_completed=0
timed_out=0
process_exited_early=0
echo "Waiting for benchmark completion (timeout: ${max_wait}s)..."
while [ "$wait_count" -lt "$max_wait" ]; do
    shard0_done=0
    shard1_done=0
    if [ -f "$log_file0" ] && grep -q "agg_persist_throughput" "$log_file0" 2>/dev/null; then
        shard0_done=1
    fi
    if [ -f "$log_file1" ] && grep -q "agg_persist_throughput" "$log_file1" 2>/dev/null; then
        shard1_done=1
    fi
    if [ "$shard0_done" -eq 1 ] && [ "$shard1_done" -eq 1 ]; then
        echo "Both benchmarks completed after ${wait_count}s"
        benchmark_completed=1
        sleep 2
        break
    fi
    shard0_alive=1
    shard1_alive=1
    if ! kill -0 "$SHARD0_PID" 2>/dev/null; then shard0_alive=0; fi
    if ! kill -0 "$SHARD1_PID" 2>/dev/null; then shard1_alive=0; fi
    if { [ "$shard0_alive" -eq 0 ] && [ "$shard0_done" -eq 0 ]; } || \
       { [ "$shard1_alive" -eq 0 ] && [ "$shard1_done" -eq 0 ]; }; then
        sleep 1
        if grep -q "agg_persist_throughput" "$log_file0" 2>/dev/null; then shard0_done=1; fi
        if grep -q "agg_persist_throughput" "$log_file1" 2>/dev/null; then shard1_done=1; fi
        if [ "$shard0_done" -eq 1 ] && [ "$shard1_done" -eq 1 ]; then
            echo "Both benchmarks completed after ${wait_count}s (processes exited after writing results)"
            benchmark_completed=1
            sleep 1
            break
        fi
        echo "Shard process exited unexpectedly (shard0_alive=$shard0_alive, shard1_alive=$shard1_alive)"
        process_exited_early=1
        break
    fi
    sleep 1
    wait_count=$((wait_count + 1))
    if [ $((wait_count % 10)) -eq 0 ]; then
        echo "  ... waiting (${wait_count}s elapsed, shard0=$shard0_done, shard1=$shard1_done)"
    fi
done
if [ "$wait_count" -ge "$max_wait" ] && [ "$benchmark_completed" -eq 0 ]; then
    echo "Warning: Benchmarks did not complete within ${max_wait}s timeout"
    timed_out=1
fi

echo "Stopping shards..."
kill $SHARD0_PID $SHARD1_PID 2>/dev/null || true
sleep 2
kill -9 $SHARD0_PID $SHARD1_PID 2>/dev/null || true
wait $SHARD0_PID $SHARD1_PID 2>/dev/null

echo ""
echo "========================================="
echo "Checking test results..."
echo "========================================="

failed=0

if [ "$process_exited_early" -eq 1 ]; then
    echo "  ✗ Shard process exited before benchmark completion"
    failed=1
fi
if [ "$timed_out" -eq 1 ]; then
    echo "  ✗ Benchmarks timed out"
    failed=1
fi

echo ""
echo "Checking live migration:"
echo "-----------------"
if [ "$migration_ok" -eq 1 ]; then
    echo "  ✓ mako_admin migration committed (ok=1 moved=10)"
else
    echo "  ✗ mako_admin migration failed: ${admin_output}"
    failed=1
fi
if grep -aq "MigrationAdmin: committed" "$log_file0" 2>/dev/null; then
    echo "  ✓ Shard-0 master logged the commit"
    grep -a "MigrationAdmin: committed" "$log_file0" | tail -n 1 | sed 's/^/    /'
else
    echo "  ✗ Shard-0 master did not log a commit"
    failed=1
fi
if grep -aq "seeded 30 demo rows" "$log_file1" 2>/dev/null; then
    echo "  ✓ Shard 1 seeded the migratable rows"
else
    echo "  ✗ Shard 1 did not seed (data plane bring-up failed?)"
    failed=1
fi
# Cutover propagation: shard 1's ConfigWatcher (mako-config addressing) should
# reach shard 0's config service. Informational unless it logged a hard failure.
if grep -aq "watching shard-0 config" "$log_file1" 2>/dev/null; then
    echo "  ✓ Shard 1 watcher connected to shard-0 config service"
else
    echo "  (i) Shard 1 watcher line not found (propagation not exercised)"
fi

for i in 0 1; do
    log="${log_prefix}_shard${i}-$trd.log"
    echo ""
    echo "Checking $log:"
    echo "-----------------"
    if [ ! -f "$log" ]; then
        echo "  ✗ Log file not found"
        failed=1
        continue
    fi
    if [ "$i" -eq 0 ]; then
        if grep -q "TPC-C Sharding: Initialized policy" "$log"; then
            echo "  ✓ TPC-C sharding policy initialized"
        else
            echo "  ✗ TPC-C sharding policy not initialized"
            failed=1
        fi
    fi
    if grep -q "agg_persist_throughput" "$log"; then
        echo "  ✓ Found 'agg_persist_throughput'"
        grep "agg_persist_throughput" "$log" | tail -n 1 | sed 's/^/    /'
    else
        echo "  ✗ 'agg_persist_throughput' not found"
        failed=1
    fi
    if grep -q "NewOrder_remote_abort_ratio:" "$log"; then
        abort_ratio=$(grep "NewOrder_remote_abort_ratio:" "$log" | tail -n 1 | awk '{print $2}')
        abort_value=$(echo "$abort_ratio" | sed 's/%//')
        if awk "BEGIN {exit !($abort_value < 20)}"; then
            echo "  ✓ NewOrder_remote_abort_ratio: $abort_ratio (< 20%)"
        else
            echo "  ✗ NewOrder_remote_abort_ratio: $abort_ratio (>= 20%)"
            failed=1
        fi
    else
        echo "  ✗ NewOrder_remote_abort_ratio not found"
        failed=1
    fi
done

echo ""
echo "========================================="
if [ $failed -eq 0 ]; then
    echo "All checks passed!"
    echo "========================================="
    exit 0
else
    echo "Some checks failed!"
    echo "========================================="
    tail -n 15 "$log_file0" "$log_file1"
    exit 1
fi
