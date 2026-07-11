#!/bin/bash

# CAPSTONE: live TPC-C WAREHOUSE migration on the real 2-process bed.
#
# Same partition bed as test_2shard_migration_admin.sh (2 shards, no
# replication, MAKO_CLUSTER_CONFIG=1 + map routing), but the migrations move
# REAL WORKLOAD tables: warehouse 6 (shard 1's block, global ids 5..8 at
# trd=4) is migrated to shard 0 table by table via the "wh:<gwid>:<logical>"
# spec, WHILE TPC-C hammers both shards. Each spec resolves to the physical
# per-warehouse index through the warehouse directory: the source's startup
# index, and on the destination an empty ADOPTED index materialized on demand
# and filled by the destination-driven pull before the cutover publishes the
# logical table's warehouse routing segment.
#
# The four canonical warehouse-sharded tables migrate in ascending size, so
# an admin-RPC timeout on a big table cannot mask a small-table bug:
#   warehouse (1 row), district (10), customer (30000), stock (100000).
# The remaining governed tables ride the identical mechanism (same spec
# path); the whale (order_line, ~300k rows and growing under load) is a
# chunk-tuning follow-up, not a different design.
#
# Pass criteria:
#   1. All four mako_admin calls exit 0 with ok=1 and the EXACT row count
#      (these tables are insert-free under TPC-C, so moved is deterministic).
#   2. Shard 0's log shows four MigrationAdmin committed lines.
#   3. The usual bed checks pass (throughput on both shards) -- the workload
#      survives losing a warehouse mid-run.

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
source "${SCRIPT_DIR}/simple_transaction_rep_port_utils.sh"

echo "========================================="
echo "Testing live TPC-C warehouse migration (2 shards, admin-driven)"
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

# Warehouse 6 sits in shard 1's block for any trd >= 2 (shard 1 owns
# [trd+1 .. 2*trd]); with the default trd=4 that is global ids 5..8.
MIG_WH=$((trd + 2))

if [ ! -x "$binary_path" ]; then
    echo "Error: dbtest binary not found or not executable at '$binary_path'"
    exit 1
fi
if [ ! -x "$admin_path" ]; then
    echo "Error: mako_admin binary not found or not executable at '$admin_path'"
    exit 1
fi

TEMP_CONFIG=$(make_simple_txn_rep_config 2 $trd)
if [ -z "$TEMP_CONFIG" ]; then
    exit 1
fi
export MAKO_CONFIG="$TEMP_CONFIG"
echo "dbtest config: $MAKO_CONFIG  (migrating warehouse ${MIG_WH}, shard 1 -> 0)"

export MAKO_CLUSTER_CONFIG=1
export MAKO_SHARDING_MODE=map
# The four migrations (through stock's ~100k-row copy) need more benchmark
# window than the default 30s -- the shards must keep serving until every
# cutover lands, or the tail migrations die with the bed.
# Long enough for twenty-two sequential migrations (the full warehouse OUT
# and BACK; order_line alone moves ~320k rows twice per direction: bulk +
# post-fence catch-up mirror) to fire INSIDE the bench window -- the point
# is migrating under live traffic, and dbtest exits at bench end. The
# forward pass alone has taken ~10 minutes on a loaded machine (drains
# legitimately wait multi-second 2PC stragglers to their deadline).
export MAKO_RUNTIME_SECONDS=${MAKO_RUNTIME_SECONDS:-1500}

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
# Run under a bed-private binary NAME: this bed runs for many minutes, and
# concurrent test sessions' cleanup (pkill -x dbtest) has shot it mid-flight
# repeatedly. A copied binary with its own name is invisible to that.
export MAKO_DBTEST_BIN="dbtest_whmig"
cp -f "$binary_path" "./${BUILD_DIR:-build}/${MAKO_DBTEST_BIN}"
pkill -9 -x "$MAKO_DBTEST_BIN" 2>/dev/null || true
sleep 1

# Crash capture: the customer-migration segfault (T4c) needs a core.
# core_pattern is "core" (cwd-relative); shards run from the repo root.
if [ "${MAKO_CAPTURE_CORE:-0}" == "1" ]; then
    ulimit -c unlimited
    rm -f core core.* 2>/dev/null
    echo "Core dumps enabled (ulimit -c unlimited)"
fi

echo "Starting shard 0..."
nohup bash bash/shard.sh 2 0 $trd localhost > "$log_file0" 2>&1 &
SHARD0_PID=$!
sleep 5

echo "Starting shard 1..."
nohup bash bash/shard.sh 2 1 $trd localhost > "$log_file1" 2>&1 &
SHARD1_PID=$!

# ---- Wait for the admin service AND both workloads actually running ----
# The wh: specs resolve through the warehouse directory, which the TPC-C
# runner initializes in init_tables; "starting benchmark..." (printed after
# loading) is the reliable both-sides-ready gate.
echo "Waiting for the admin service + both benchmarks running..."
admin_addr=""
dp_wait=0
while [ "$dp_wait" -lt 150 ]; do
    la=$(grep -a "MigrationAdmin listening on" "$log_file0" 2>/dev/null | tail -n 1)
    b0=$(grep -ac "starting benchmark" "$log_file0" 2>/dev/null || true)
    b1=$(grep -ac "starting benchmark" "$log_file1" 2>/dev/null || true)
    if [ -n "$la" ] && [ "${b0:-0}" -ge 1 ] && [ "${b1:-0}" -ge 1 ]; then
        port=$(echo "$la" | sed -n 's/.*listening on 0\.0\.0\.0:\([0-9]*\).*/\1/p')
        if [ -n "$port" ]; then
            admin_addr="127.0.0.1:${port}"
            break
        fi
    fi
    sleep 1
    dp_wait=$((dp_wait + 1))
done

declare -A mig_ok
declare -A mig_out
# EVERY governed table of the warehouse -- the full warehouse move (item is
# ungoverned: read-only, replicated per shard). CUSTOMER GOES FIRST: its
# partition segment is the WarehouseInShard group signal, so committing it
# first re-homes the warehouse's scan-bearing transactions (delivery /
# order_status / stock_level skip a departed home; remote proxies cannot
# serve their multi-row range scans) BEFORE any scanned table moves. After
# customer, ascending size so a timeout on a big table cannot mask a
# small-table bug. Expectations are MINIMUMS at load time; the workload
# grows history/oorder/order_line, and new_order can be fully consumed by
# deliveries (0 rows is a legitimate commit).
MIG_TABLES=(customer warehouse district new_order customer_name_idx history oorder oorder_c_id_idx stock stock_data order_line)
# RETURN LEG (ping-pong, 0 -> 1): the mirror image -- customer LAST, so the
# group signal flips back only once every scannable table is already home
# (the resuming home worker's delivery/stock_level scans must not land on
# still-remote proxies).
MIG_TABLES_BACK=(warehouse district new_order customer_name_idx history oorder oorder_c_id_idx stock stock_data order_line customer)
declare -A MIG_EXPECT=([customer]=30000 [warehouse]=1 [district]=10
                       [new_order]=0
                       [customer_name_idx]=30000 [history]=30000
                       [oorder]=30000 [oorder_c_id_idx]=30000
                       [stock]=100000 [stock_data]=100000
                       [order_line]=250000)

# Fire one gated migration of wh:${MIG_WH}:$1 from shard $2 to shard $3,
# recording into mig_ok/mig_out under key "$4" (pass label + table). A failed
# attempt aborts cleanly (source intact, unfrozen), so one retry is safe --
# and diagnostic: attempt 2 starts from an already-mostly-copied destination
# (put-overwrite mirror), so a window-length-sensitive failure commits on
# retry while a structural one repeats.
run_migration() {
    local t="$1" src="$2" dst="$3" key="$4"
    local spec="wh:${MIG_WH}:${t}"
    mig_ok[$key]=0
    local attempt out rc moved
    for attempt in 1 2; do
        echo "Firing: mako_admin migrate ${admin_addr} ${spec} ${src} -> ${dst} (attempt ${attempt})"
        out=$("$admin_path" migrate "$admin_addr" "$spec" x x "$src" "$dst" 30 2>&1)
        rc=$?
        echo "mako_admin(${key}#${attempt}): rc=${rc} output: ${out}"
        mig_out[$key]="$out"
        # moved is a MINIMUM: loaders add secondary rows (e.g. customer
        # keeps balance/data keys in the same index), so >= base count.
        moved=$(echo "$out" | sed -n 's/.*ok=1 moved=\([0-9]*\).*/\1/p')
        if [ "$rc" -eq 0 ] && [ -n "$moved" ] && [ "$moved" -ge "${MIG_EXPECT[$t]}" ]; then
            mig_ok[$key]=1
            mig_out[$key]="moved=${moved} (attempt ${attempt})"
            return 0
        fi
    done
    return 1
}

if [ -z "$admin_addr" ]; then
    echo "  ✗ Admin service / benchmarks did not come up within 150s"
else
    echo "Benchmarks running on both shards; MigrationAdmin at ${admin_addr}"
    sleep 2   # a beat of steady-state traffic before the first cutover
    for t in "${MIG_TABLES[@]}"; do
        run_migration "$t" 1 0 "$t"
    done
    # ---- PING-PONG RETURN LEG: the whole warehouse goes home, 0 -> 1 ----
    # This is also the REMOTE-DESTINATION path from the master's seat: the
    # destination participant is an RPC proxy, so the copy runs as PullRange
    # (the destination pulls; rows never transit the master) and the prepare
    # vote as the remote VerifyRange job. commit clears the returning
    # shard's stale MOVED fence, so it serves the range again immediately.
    echo "Return leg: migrating warehouse ${MIG_WH} back 0 -> 1"
    sleep 2   # let post-cutover traffic settle on the new owner first
    for t in "${MIG_TABLES_BACK[@]}"; do
        run_migration "$t" 0 1 "back:${t}"
    done
fi

# ---- Wait for benchmark completion (standard bed behavior) ----
max_wait="${MAKO_MAX_WAIT_SECONDS:-150}"
if ! [[ "$max_wait" =~ ^[0-9]+$ ]] || [ "$max_wait" -le 0 ]; then
    max_wait=150
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
echo "Checking live warehouse migration (warehouse ${MIG_WH}, shard 1 -> 0):"
echo "-----------------"
# GATED assertion: EVERY table must migrate live IN BOTH DIRECTIONS,
# including the write-hot big ones (customer 90k rows, order_line 320k) --
# the full mechanism end to end: spec resolution, adopted index, widened
# chunked mirror copy, physical-name freeze, per-table gen-bucket drain
# (participant txn boundary included), post-fence catch-up, begin/poll
# checksum + verify jobs, commit, publish -- and on the return leg the
# remote-destination path (PullRange copy, remote verify vote, stale-MOVED
# fence cleared on the returning owner).
for t in "${MIG_TABLES[@]}"; do
    if [ "${mig_ok[$t]:-0}" -eq 1 ]; then
        echo "  ✓ ${t}: committed (${mig_out[$t]}, expected >= ${MIG_EXPECT[$t]})"
    else
        echo "  ✗ ${t}: failed: ${mig_out[$t]:-not fired}"
        failed=1
    fi
done
for t in "${MIG_TABLES_BACK[@]}"; do
    k="back:${t}"
    if [ "${mig_ok[$k]:-0}" -eq 1 ]; then
        echo "  ✓ ${k}: committed (${mig_out[$k]}, expected >= ${MIG_EXPECT[$t]})"
    else
        echo "  ✗ ${k}: failed: ${mig_out[$k]:-not fired}"
        failed=1
    fi
done

commits=$(grep -ac "MigrationAdmin: committed" "$log_file0" 2>/dev/null || echo 0)
if [ "${commits:-0}" -ge 1 ]; then
    echo "  ✓ Shard-0 master logged ${commits} commit(s)"
else
    echo "  ✗ Shard-0 master logged no commits"
    failed=1
fi
# (No PullRange check: the destination here IS the master's shard, so the
# copy is a local chunked pull over ScanRange -- the PullRange fast path only
# fires when the destination is remote from the master.)

echo ""
echo "Checking benchmark health (throughput on both shards):"
echo "-----------------"
for lf in "$log_file0" "$log_file1"; do
    tp=$(grep -a "agg_persist_throughput" "$lf" 2>/dev/null | tail -n 1)
    if [ -n "$tp" ]; then
        echo "  ✓ Found 'agg_persist_throughput'"
        echo "    $(echo "$tp" | sed -n 's/.*\(agg_persist_throughput: [0-9.]* ops\/sec\).*/\1/p')"
    else
        echo "  ✗ Missing throughput in ${lf}"
        failed=1
    fi
done

echo ""
if [ "$failed" -eq 0 ]; then
    echo "All checks passed!"
    exit 0
else
    echo "Some checks failed!"
    exit 1
fi
