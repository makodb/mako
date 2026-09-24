#!/bin/bash

# Run shard1Replication (Paxos) 10 times and produce a detailed benchmark report.
# Usage: bash run_10x_shard1_paxos.sh [num_runs]
#   num_runs: number of benchmark runs (default: 10)
#
# NOTE: This script mirrors run_10x_shard1_raft.sh exactly in structure,
# metrics, and statistics — only the CI command and log filenames differ.
# This ensures Paxos vs Raft results are directly comparable in the thesis.

set -o pipefail

NUM_RUNS=${1:-10}
TIMESTAMP=$(date +%Y%m%d_%H%M%S)
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BENCH_ROOT="${SCRIPT_DIR}/results/benchmarks/paxos"
RESULTS_DIR="${BENCH_ROOT}/shard1_paxos_${TIMESTAMP}"
REPORT_FILE="${RESULTS_DIR}/report.txt"
CSV_FILE="${RESULTS_DIR}/results.csv"
LATEST_LINK="${BENCH_ROOT}/shard1_paxos_latest"

# ci.sh shard1Replication calls examples/test_1shard_replication.sh which produces:
# test_1shard_replication.sh_shard0-{node}-{trd}.log in CWD
LEADER_LOG="${SCRIPT_DIR}/test_1shard_replication.sh_shard0-localhost-6.log"
FOLLOWER_LOG="${SCRIPT_DIR}/test_1shard_replication.sh_shard0-p1-6.log"
FOLLOWER_P2_LOG="${SCRIPT_DIR}/test_1shard_replication.sh_shard0-p2-6.log"
LEARNER_LOG="${SCRIPT_DIR}/test_1shard_replication.sh_shard0-learner-6.log"

mkdir -p "$RESULTS_DIR"

# Update "latest" symlink for easy access
ln -sfn "$RESULTS_DIR" "$LATEST_LINK"

echo "============================================================"
echo "  Mako Paxos Benchmark"
echo "  Configuration: 1 shard, 6 threads, 4 replicas (3+learner), TPC-C"
echo "  Runs: $NUM_RUNS"
echo "  Started: $(date)"
echo "  Results: $RESULTS_DIR/"
echo "============================================================"
echo ""

# CSV header — all metrics we extract per run
cat > "$CSV_FILE" << 'CSVHEADER'
run,exit_code,agg_persist_throughput,avg_per_core_persist_throughput,agg_throughput,avg_latency_ms,avg_persist_latency_ms,agg_abort_rate,replay_batch_p1,replay_batch_p2,neworder_local_commit_latency_ms,neworder_local_abort_ratio,payment_local_commit_latency_ms,payment_local_abort_ratio,delivery_local_commit_latency_ms,orderstatus_local_commit_latency_ms,stocklevel_local_commit_latency_ms,neworder_remote_abort_ratio,payment_remote_abort_ratio
CSVHEADER

# Arrays for per-run values (used for summary stats)
declare -a arr_throughput
declare -a arr_per_core_throughput
declare -a arr_latency
declare -a arr_persist_latency
declare -a arr_abort_rate
declare -a arr_replay_p1
declare -a arr_replay_p2
declare -a arr_neworder_lat
declare -a arr_payment_lat
declare -a arr_delivery_lat
declare -a arr_orderstatus_lat
declare -a arr_stocklevel_lat
declare -a arr_neworder_abort
declare -a arr_payment_abort

successful_runs=0
failed_runs=0
start_time=$(date +%s)

# Helper: extract a numeric value from a log line matching a keyword.
# Grabs the first number after the colon. Filters out nan/-nan/inf.
extract_metric() {
    local file="$1" keyword="$2"
    local val
    # Get the part after "keyword", then extract first number (int or float, incl. scientific notation)
    val=$(grep "$keyword" "$file" 2>/dev/null | tail -1 | sed "s/.*${keyword}//" | grep -oP '[-+]?(\d+\.?\d*|\.\d+)([eE][-+]?\d+)?' | head -1)
    # Filter out nan / -nan / inf
    if [ -z "$val" ] || echo "$val" | grep -qiE '^-?nan$|^-?inf$'; then
        echo ""
    else
        echo "$val"
    fi
}

# Helper: extract replay_batch count from a follower log
extract_replay_batch() {
    local file="$1"
    if [ -f "$file" ]; then
        grep "replay_batch:" "$file" | tail -1 | sed -n 's/.*replay_batch:\([0-9]*\).*/\1/p'
    fi
}

for run in $(seq 1 "$NUM_RUNS"); do
    echo "------------------------------------------------------------"
    echo "  Run $run / $NUM_RUNS  ($(date +%H:%M:%S))"
    echo "------------------------------------------------------------"

    # Clean up leftover processes
    pkill -9 -f "build/dbtest" 2>/dev/null || true
    # Wait for processes to die and ports to leave TIME_WAIT
    sleep 5

    # Remove stale log files so we don't accidentally read old data
    rm -f "$LEADER_LOG" "$FOLLOWER_LOG" "$FOLLOWER_P2_LOG" "$LEARNER_LOG" 2>/dev/null

    # Run the test (Paxos)
    bash ./ci/ci.sh shard1Replication > "${RESULTS_DIR}/run_${run}.log" 2>&1
    exit_code=$?

    # Preserve raw logs for this run
    cp "$LEADER_LOG" "${RESULTS_DIR}/run_${run}_leader.log" 2>/dev/null || true
    cp "$FOLLOWER_LOG" "${RESULTS_DIR}/run_${run}_follower_p1.log" 2>/dev/null || true
    cp "$FOLLOWER_P2_LOG" "${RESULTS_DIR}/run_${run}_follower_p2.log" 2>/dev/null || true
    cp "$LEARNER_LOG" "${RESULTS_DIR}/run_${run}_learner.log" 2>/dev/null || true

    # ---- Extract all metrics from leader log ----
    throughput=""
    per_core_throughput=""
    agg_throughput=""
    avg_latency=""
    avg_persist_latency=""
    abort_rate=""
    replay_p1=""
    replay_p2=""
    no_local_lat="" no_local_abort=""
    pay_local_lat="" pay_local_abort=""
    del_local_lat=""
    os_local_lat=""
    sl_local_lat=""
    no_remote_abort="" pay_remote_abort=""

    if [ -f "$LEADER_LOG" ]; then
        throughput=$(extract_metric "$LEADER_LOG" "agg_persist_throughput:")
        per_core_throughput=$(extract_metric "$LEADER_LOG" "avg_per_core_persist_throughput:")
        agg_throughput=$(extract_metric "$LEADER_LOG" "agg_throughput:")
        avg_latency=$(extract_metric "$LEADER_LOG" "avg_latency:")
        avg_persist_latency=$(extract_metric "$LEADER_LOG" "avg_persist_latency:")
        abort_rate=$(extract_metric "$LEADER_LOG" "agg_abort_rate:")

        # Per-transaction latencies
        no_local_lat=$(extract_metric "$LEADER_LOG" "NewOrder_local_commit_latency:")
        no_local_abort=$(extract_metric "$LEADER_LOG" "NewOrder_local_abort_ratio:")
        pay_local_lat=$(extract_metric "$LEADER_LOG" "Payment_local_commit_latency:")
        pay_local_abort=$(extract_metric "$LEADER_LOG" "Payment_local_abort_ratio:")
        del_local_lat=$(extract_metric "$LEADER_LOG" "Delivery_local_commit_latency:")
        os_local_lat=$(extract_metric "$LEADER_LOG" "OrderStatus_local_commit_latency:")
        sl_local_lat=$(extract_metric "$LEADER_LOG" "StockLevel_local_commit_latency:")

        # Remote abort ratios (may be -nan for single shard — extract_metric handles it)
        no_remote_abort=$(extract_metric "$LEADER_LOG" "NewOrder_remote_abort_ratio:")
        pay_remote_abort=$(extract_metric "$LEADER_LOG" "Payment_remote_abort_ratio:")
    fi

    # Follower replay batches (both followers)
    replay_p1=$(extract_replay_batch "$FOLLOWER_LOG")
    replay_p2=$(extract_replay_batch "$FOLLOWER_P2_LOG")

    # Display
    disp_tp="${throughput:-N/A}"
    disp_lat="${avg_latency:-N/A}"
    disp_replay_p1="${replay_p1:-N/A}"

    # Count as successful if we got throughput data
    if [ -n "$throughput" ]; then
        echo "    Throughput:  $disp_tp ops/sec"
        echo "    Avg Latency: $disp_lat ms"
        echo "    Replay (p1): $disp_replay_p1 batches"
        echo "    Status:      PASS"
        ((successful_runs++))
    else
        echo "    Throughput:  N/A"
        echo "    Status:      FAIL (exit=$exit_code)"
        ((failed_runs++))
    fi

    # Write CSV row
    echo "${run},${exit_code},${throughput},${per_core_throughput},${agg_throughput},${avg_latency},${avg_persist_latency},${abort_rate},${replay_p1},${replay_p2},${no_local_lat},${no_local_abort},${pay_local_lat},${pay_local_abort},${del_local_lat},${os_local_lat},${sl_local_lat},${no_remote_abort},${pay_remote_abort}" >> "$CSV_FILE"

    # Accumulate for stats (only runs with data)
    [ -n "$throughput" ]          && arr_throughput+=("$throughput")
    [ -n "$per_core_throughput" ] && arr_per_core_throughput+=("$per_core_throughput")
    [ -n "$avg_latency" ]         && arr_latency+=("$avg_latency")
    [ -n "$avg_persist_latency" ] && arr_persist_latency+=("$avg_persist_latency")
    [ -n "$abort_rate" ]          && arr_abort_rate+=("$abort_rate")
    [ -n "$replay_p1" ]           && arr_replay_p1+=("$replay_p1")
    [ -n "$replay_p2" ]           && arr_replay_p2+=("$replay_p2")
    [ -n "$no_local_lat" ]        && arr_neworder_lat+=("$no_local_lat")
    [ -n "$pay_local_lat" ]       && arr_payment_lat+=("$pay_local_lat")
    [ -n "$del_local_lat" ]       && arr_delivery_lat+=("$del_local_lat")
    [ -n "$os_local_lat" ]        && arr_orderstatus_lat+=("$os_local_lat")
    [ -n "$sl_local_lat" ]        && arr_stocklevel_lat+=("$sl_local_lat")
    [ -n "$no_local_abort" ]      && arr_neworder_abort+=("$no_local_abort")
    [ -n "$pay_local_abort" ]     && arr_payment_abort+=("$pay_local_abort")

    # Clean up between runs
    pkill -9 -f "build/dbtest" 2>/dev/null || true
    sleep 5
done

end_time=$(date +%s)
total_elapsed=$(( end_time - start_time ))
avg_run_time=$(( total_elapsed / NUM_RUNS ))

# ==========================================================================
#  Summary statistics (awk function to compute mean, stdev, median, etc.)
# ==========================================================================
compute_stats() {
    local label="$1"
    local unit="$2"
    shift 2
    local values=("$@")

    if [ ${#values[@]} -eq 0 ]; then
        echo "  $label: no data"
        echo ""
        return
    fi

    printf '%s\n' "${values[@]}" | awk -v label="$label" -v unit="$unit" '
    BEGIN { min=1e18; max=-1e18; sum=0; n=0 }
    {
        n++; sum+=$1; vals[n]=$1
        if ($1 < min) min=$1
        if ($1 > max) max=$1
    }
    END {
        mean = sum/n
        sumsq = 0
        for (i=1; i<=n; i++) sumsq += (vals[i]-mean)^2
        stdev = (n>1) ? sqrt(sumsq/(n-1)) : 0
        cv = (mean>0) ? (stdev/mean)*100 : 0
        ci95 = (n>1) ? 1.96 * stdev / sqrt(n) : 0

        # Sort for median and percentiles
        for (i=1; i<=n; i++)
            for (j=i+1; j<=n; j++)
                if (vals[i] > vals[j]) { t=vals[i]; vals[i]=vals[j]; vals[j]=t }

        if (n%2 == 0) median = (vals[n/2] + vals[n/2+1]) / 2
        else median = vals[(n+1)/2]

        p5  = vals[int(n*0.05)+1]
        p95 = vals[int(n*0.95)+1]

        printf "  %-40s\n", label
        printf "    Mean:            %12.2f %s\n", mean, unit
        printf "    Median:          %12.2f %s\n", median, unit
        printf "    Stdev:           %12.2f %s\n", stdev, unit
        printf "    CV:              %11.1f %%\n", cv
        printf "    95%% CI:          +/- %.2f %s\n", ci95, unit
        printf "    Min:             %12.2f %s\n", min, unit
        printf "    Max:             %12.2f %s\n", max, unit
        printf "    P5:              %12.2f %s\n", p5, unit
        printf "    P95:             %12.2f %s\n", p95, unit
        printf "    Range:           %12.2f %s\n", max-min, unit
        printf "    N:               %12d\n", n
        printf "\n"
    }'
}

# ==========================================================================
#  Generate report
# ==========================================================================
{
echo "============================================================"
echo "  MAKO PAXOS BENCHMARK REPORT"
echo "============================================================"
echo ""
echo "  Date:           $(date)"
echo "  Configuration:  1 shard, 6 threads (warehouses), TPC-C"
echo "  Replication:    Multi-Paxos (4 replicas: 3 voters + 1 learner)"
echo "  Transport:      srpc/rpc (TCP/IP)"
echo "  Host:           $(hostname)"
echo "  Runs:           $NUM_RUNS total ($successful_runs passed, $failed_runs failed)"
echo "  Total time:     ${total_elapsed}s (~${avg_run_time}s per run)"
echo "  Git branch:     $(git -C "$SCRIPT_DIR" rev-parse --abbrev-ref HEAD 2>/dev/null || echo 'unknown')"
echo "  Git commit:     $(git -C "$SCRIPT_DIR" rev-parse --short HEAD 2>/dev/null || echo 'unknown')"
echo ""

echo "============================================================"
echo "  PER-RUN RESULTS"
echo "============================================================"
echo ""
printf "  %-4s  %-8s  %-16s  %-10s  %-12s  %-10s  %-10s\n" "Run" "Status" "Throughput" "Latency" "AbortRate" "Replay-p1" "Replay-p2"
printf "  %-4s  %-8s  %-16s  %-10s  %-12s  %-10s  %-10s\n" "---" "------" "----------" "-------" "---------" "---------" "---------"

# Re-read CSV to print table
tail -n +2 "$CSV_FILE" | while IFS=',' read -r run ec tp pct at lat plat ar rp1 rp2 rest; do
    if [ -n "$tp" ]; then
        status="PASS"
    else
        status="FAIL"
    fi
    printf "  %-4s  %-8s  %10s ops/s  %7s ms  %8s /s  %8s  %8s\n" \
        "$run" "$status" "${tp:-N/A}" "${lat:-N/A}" "${ar:-N/A}" "${rp1:-N/A}" "${rp2:-N/A}"
done
echo ""

echo "============================================================"
echo "  THROUGHPUT STATISTICS"
echo "============================================================"
echo ""
compute_stats "Aggregate Persist Throughput" "ops/sec" "${arr_throughput[@]}"
compute_stats "Per-Core Persist Throughput" "ops/sec/core" "${arr_per_core_throughput[@]}"

echo "============================================================"
echo "  LATENCY STATISTICS"
echo "============================================================"
echo ""
compute_stats "Average Latency" "ms" "${arr_latency[@]}"
compute_stats "Average Persist Latency" "ms" "${arr_persist_latency[@]}"

echo "============================================================"
echo "  PER-TRANSACTION COMMIT LATENCY (LOCAL)"
echo "============================================================"
echo ""
compute_stats "NewOrder (45% of mix)" "ms" "${arr_neworder_lat[@]}"
compute_stats "Payment (43% of mix)" "ms" "${arr_payment_lat[@]}"
compute_stats "Delivery (4% of mix)" "ms" "${arr_delivery_lat[@]}"
compute_stats "OrderStatus (4% of mix)" "ms" "${arr_orderstatus_lat[@]}"
compute_stats "StockLevel (4% of mix)" "ms" "${arr_stocklevel_lat[@]}"

echo "============================================================"
echo "  ABORT STATISTICS"
echo "============================================================"
echo ""
compute_stats "Aggregate Abort Rate" "aborts/sec" "${arr_abort_rate[@]}"
compute_stats "NewOrder Local Abort Ratio" "" "${arr_neworder_abort[@]}"
compute_stats "Payment Local Abort Ratio" "" "${arr_payment_abort[@]}"

echo "============================================================"
echo "  REPLICATION HEALTH"
echo "============================================================"
echo ""
compute_stats "Follower Replay Batches (p1)" "batches" "${arr_replay_p1[@]}"
compute_stats "Follower Replay Batches (p2)" "batches" "${arr_replay_p2[@]}"

echo "============================================================"
echo "  STABILITY ASSESSMENT"
echo "============================================================"
echo ""
if [ ${#arr_throughput[@]} -gt 1 ]; then
    printf '%s\n' "${arr_throughput[@]}" | awk '
    BEGIN { sum=0; n=0 }
    { n++; sum+=$1; vals[n]=$1 }
    END {
        mean = sum/n
        sumsq = 0
        for (i=1; i<=n; i++) sumsq += (vals[i]-mean)^2
        stdev = sqrt(sumsq/(n-1))
        cv = (stdev/mean)*100

        if (cv < 5)       grade = "EXCELLENT"
        else if (cv < 10) grade = "GOOD"
        else if (cv < 20) grade = "MODERATE"
        else if (cv < 35) grade = "HIGH VARIANCE"
        else               grade = "UNSTABLE"

        printf "  Throughput CV: %.1f%% -> %s\n", cv, grade
        printf "\n"
        printf "  Interpretation:\n"
        printf "    < 5%%   EXCELLENT   Highly reproducible, thesis-ready\n"
        printf "    < 10%%  GOOD        Minor run-to-run variance\n"
        printf "    < 20%%  MODERATE    Some variance, investigate outliers\n"
        printf "    < 35%%  HIGH        Possible bimodal behavior\n"
        printf "    > 35%%  UNSTABLE    Likely systemic issue (election storms, contention)\n"
    }'
else
    echo "  Not enough data for stability assessment (need >= 2 runs)"
fi
echo ""

echo "============================================================"
echo "  FILES"
echo "============================================================"
echo ""
echo "  Results dir:     $RESULTS_DIR"
echo "  Latest link:     $LATEST_LINK"
echo "  Report:          ${RESULTS_DIR}/report.txt"
echo "  CSV data:        ${RESULTS_DIR}/results.csv"
echo "  Run logs:        ${RESULTS_DIR}/run_*.log"
echo "  Leader logs:     ${RESULTS_DIR}/run_*_leader.log"
echo "  Follower logs:   ${RESULTS_DIR}/run_*_follower_p*.log"
echo "  Learner logs:    ${RESULTS_DIR}/run_*_learner.log"

} 2>&1 | tee "$REPORT_FILE"

echo ""
echo "Done. Full report saved to: $REPORT_FILE"
