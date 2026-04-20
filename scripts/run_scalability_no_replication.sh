#!/bin/bash

# Scalability sweep for Mako WITHOUT the replication layer.
# Runs a single shard (no Paxos/Raft, no followers) across a range of
# worker thread counts to characterize raw single-shard throughput.
#
# Usage:
#   bash scripts/run_scalability_no_replication.sh                       # defaults
#   bash scripts/run_scalability_no_replication.sh --threads "1 2 4 8 16 24"
#   bash scripts/run_scalability_no_replication.sh --runs 5
#   bash scripts/run_scalability_no_replication.sh --batch-size 200
#
# Notes:
#   - 1 shard, NO replication (single dbtest process per run).
#   - Thread count == warehouse count (uses
#     src/mako/config/local-shards1-warehouses${trd}.yml).
#   - Configs only exist for thread counts 1..32; values outside that range
#     are skipped with a warning.

set -o pipefail

# ============================================================
#  Parse arguments
# ============================================================
THREADS="1 2 4 8 16 24"
BATCH_SIZE=400
NUM_RUNS=3

while [[ $# -gt 0 ]]; do
    case "$1" in
        --threads)    THREADS="$2"; shift 2 ;;
        --batch-size) BATCH_SIZE="$2"; shift 2 ;;
        --runs)       NUM_RUNS="$2"; shift 2 ;;
        -h|--help)
            sed -n '3,18p' "$0"
            exit 0
            ;;
        *) echo "Unknown arg: $1"; exit 1 ;;
    esac
done

# ============================================================
#  Setup directories
# ============================================================
TIMESTAMP=$(date +%Y%m%d_%H%M%S)
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$SCRIPT_DIR" || exit 1

BENCH_ROOT="${SCRIPT_DIR}/results/benchmarks/no_replication"
RESULTS_DIR="${BENCH_ROOT}/scalability_${TIMESTAMP}"
LOGS_DIR="${RESULTS_DIR}/logs"
SUMMARY_FILE="${RESULTS_DIR}/summary.txt"
CSV_FILE="${RESULTS_DIR}/results.csv"
LATEST_LINK="${BENCH_ROOT}/scalability_latest"

mkdir -p "$LOGS_DIR"
ln -sfn "$RESULTS_DIR" "$LATEST_LINK"

BINARY="./${BUILD_DIR:-build}/dbtest"
if [ ! -x "$BINARY" ]; then
    echo "Error: dbtest binary not found at '$BINARY'"
    echo "Build it first (Docker: ./docker_build.sh build), then retry."
    exit 1
fi

# Pull in the temp-config / port-randomization helpers used by the existing
# no-replication CI test (examples/test_2shard_no_replication.sh).
source "${SCRIPT_DIR}/examples/simple_transaction_rep_port_utils.sh"

# ============================================================
#  Helpers
# ============================================================
extract_metric() {
    local file="$1" keyword="$2"
    local val
    val=$(grep "$keyword" "$file" 2>/dev/null | tail -1 | sed "s/.*${keyword}//" | grep -oP '[-+]?(\d+\.?\d*|\.\d+)([eE][-+]?\d+)?' | head -1)
    if [ -z "$val" ] || echo "$val" | grep -qiE '^-?nan$|^-?inf$'; then
        echo ""
    else
        echo "$val"
    fi
}

# ============================================================
#  CPU monitor (process + per-thread)
# ============================================================
CPU_MONITOR_PID=""
THREAD_MONITOR_PID=""

start_cpu_monitor() {
    local logfile="$1"
    local thread_logfile="$2"

    echo "# timestamp total_cpu_pct n_procs per_proc_pcts" > "$logfile"
    (
        declare -A prev_total
        while true; do
            pids=$(pgrep -u "$USER" -f "build/dbtest" 2>/dev/null || true)
            if [ -z "$pids" ]; then
                sleep 1
                continue
            fi
            declare -A curr_total
            for pid in $pids; do
                stat_file="/proc/$pid/stat"
                if [ -r "$stat_file" ]; then
                    vals=$(awk '{print $14+$15+$16+$17}' "$stat_file" 2>/dev/null)
                    [ -n "$vals" ] && curr_total[$pid]=$vals
                fi
            done
            total_pct=0
            n_procs=0
            per_proc=""
            for pid in $pids; do
                if [ -n "${curr_total[$pid]}" ] && [ -n "${prev_total[$pid]}" ]; then
                    delta=$(( ${curr_total[$pid]} - ${prev_total[$pid]} ))
                    pct=$(awk -v d="$delta" 'BEGIN { printf "%.1f", d }')
                    total_pct=$(awk -v a="$total_pct" -v b="$delta" 'BEGIN { printf "%.1f", a + b }')
                    n_procs=$((n_procs + 1))
                    per_proc="${per_proc} ${pid}:${pct}%"
                fi
            done
            if [ "$n_procs" -gt 0 ]; then
                echo "$(date +%H:%M:%S) ${total_pct} ${n_procs} ${per_proc}" >> "$logfile"
            fi
            prev_total=()
            for pid in "${!curr_total[@]}"; do
                prev_total[$pid]=${curr_total[$pid]}
            done
            sleep 1
        done
    ) &
    CPU_MONITOR_PID=$!

    echo "# timestamp total_cpu_pct n_threads per_thread_pcts" > "$thread_logfile"
    (
        declare -A prev_thread_total
        while true; do
            pids=$(pgrep -u "$USER" -f "build/dbtest" 2>/dev/null || true)
            if [ -z "$pids" ]; then
                sleep 1
                continue
            fi
            declare -A curr_thread_total
            for pid in $pids; do
                task_dir="/proc/$pid/task"
                if [ -d "$task_dir" ]; then
                    for tid in $(ls "$task_dir" 2>/dev/null); do
                        stat_file="$task_dir/$tid/stat"
                        if [ -r "$stat_file" ]; then
                            vals=$(awk '{print $14+$15}' "$stat_file" 2>/dev/null)
                            [ -n "$vals" ] && curr_thread_total["${pid}_${tid}"]=$vals
                        fi
                    done
                fi
            done
            total_pct=0
            n_threads=0
            per_thread=""
            for key in "${!curr_thread_total[@]}"; do
                if [ -n "${prev_thread_total[$key]}" ]; then
                    delta=$(( ${curr_thread_total[$key]} - ${prev_thread_total[$key]} ))
                    if [ "$delta" -gt 5 ]; then
                        pct=$(awk -v d="$delta" 'BEGIN { printf "%.0f", d }')
                        total_pct=$(awk -v a="$total_pct" -v b="$delta" 'BEGIN { printf "%.0f", a + b }')
                        n_threads=$((n_threads + 1))
                        per_thread="${per_thread} ${key}:${pct}%"
                    fi
                fi
            done
            if [ "$n_threads" -gt 0 ]; then
                echo "$(date +%H:%M:%S) ${total_pct} ${n_threads} ${per_thread}" >> "$thread_logfile"
            fi
            prev_thread_total=()
            for key in "${!curr_thread_total[@]}"; do
                prev_thread_total[$key]=${curr_thread_total[$key]}
            done
            sleep 1
        done
    ) &
    THREAD_MONITOR_PID=$!
}

stop_cpu_monitor() {
    if [ -n "$CPU_MONITOR_PID" ]; then
        kill $CPU_MONITOR_PID 2>/dev/null || true
        wait $CPU_MONITOR_PID 2>/dev/null || true
        CPU_MONITOR_PID=""
    fi
    if [ -n "$THREAD_MONITOR_PID" ]; then
        kill $THREAD_MONITOR_PID 2>/dev/null || true
        wait $THREAD_MONITOR_PID 2>/dev/null || true
        THREAD_MONITOR_PID=""
    fi
}

# ============================================================
#  Main sweep
# ============================================================
echo "============================================================"
echo "  Mako Scalability Sweep (NO replication, 1 shard)"
echo "  Threads:     $THREADS"
echo "  Batch size:  $BATCH_SIZE"
echo "  Runs/config: $NUM_RUNS"
echo "  Started:     $(date)"
echo "  Results:     $RESULTS_DIR/"
echo "  Git branch:  $(git -C "$SCRIPT_DIR" rev-parse --abbrev-ref HEAD 2>/dev/null || echo 'unknown')"
echo "  Git commit:  $(git -C "$SCRIPT_DIR" rev-parse --short HEAD 2>/dev/null || echo 'unknown')"
echo "============================================================"
echo ""

echo "threads,run,throughput_ops_sec,per_core_throughput,avg_cpu_pct,peak_cpu_pct,avg_latency_ms,avg_persist_latency_ms,agg_abort_rate,active_threads,exit_code" > "$CSV_FILE"

declare -A thread_throughputs

for trd in $THREADS; do
    src_cfg="${SCRIPT_DIR}/src/mako/config/local-shards1-warehouses${trd}.yml"
    if [ ! -f "$src_cfg" ]; then
        echo "WARNING: no config for ${trd} threads ($src_cfg); skipping"
        continue
    fi

    echo "============================================================"
    echo "  Thread count: $trd"
    echo "============================================================"

    for run in $(seq 1 "$NUM_RUNS"); do
        echo "  --- Run $run / $NUM_RUNS (threads=$trd) ---"

        # Cleanup
        pkill -9 -x dbtest 2>/dev/null || true
        USERNAME=${USER:-unknown}
        rm -rf /tmp/${USERNAME}_mako_rocksdb_shard* 2>/dev/null
        sleep 3

        # Per-run temp config with randomized ports (avoids collisions on shared hosts).
        TEMP_CONFIG=$(make_simple_txn_rep_config 1 "$trd")
        if [ -z "$TEMP_CONFIG" ]; then
            echo "    ERROR: failed to allocate temp config; skipping run"
            continue
        fi
        export MAKO_CONFIG="$TEMP_CONFIG"

        shard_log="${LOGS_DIR}/t${trd}_run${run}_shard.log"
        cpu_log="${LOGS_DIR}/t${trd}_run${run}_cpu.log"
        thread_cpu_log="${LOGS_DIR}/t${trd}_run${run}_cpu_threads.log"

        start_cpu_monitor "$cpu_log" "$thread_cpu_log"

        # Launch the single shard with NO replication
        # (bash/shard.sh args: nshard shard_idx threads cluster is_micro is_replicated)
        export MAKO_BATCH_SIZE=$BATCH_SIZE
        nohup bash bash/shard.sh 1 0 "$trd" localhost 0 0 > "$shard_log" 2>&1 &
        SHARD_PID=$!

        # Poll for completion. TPC-C data loading time scales with warehouse
        # count, so use a generous per-run timeout (was 180s — too short for 24w).
        max_wait=600
        wait_count=0
        completed=0
        while [ $wait_count -lt $max_wait ]; do
            if [ -f "$shard_log" ] && grep -q "agg_persist_throughput" "$shard_log" 2>/dev/null; then
                echo "    Benchmark completed after ${wait_count}s"
                completed=1
                sleep 2
                break
            fi
            if ! kill -0 "$SHARD_PID" 2>/dev/null; then
                # Process exited; give logs a moment to flush
                sleep 1
                if [ -f "$shard_log" ] && grep -q "agg_persist_throughput" "$shard_log" 2>/dev/null; then
                    completed=1
                fi
                break
            fi
            sleep 1
            wait_count=$((wait_count + 1))
            if [ $((wait_count % 30)) -eq 0 ]; then
                echo "    ... waiting (${wait_count}s elapsed)"
            fi
        done

        exit_code=0
        if [ $completed -ne 1 ]; then
            echo "    WARNING: timeout or early exit before agg_persist_throughput"
            exit_code=1
        fi

        stop_cpu_monitor

        # Graceful shutdown
        kill "$SHARD_PID" 2>/dev/null || true
        sleep 2
        if [ -n "${TEMP_CONFIG:-}" ]; then
            pkill -TERM -f "$TEMP_CONFIG" 2>/dev/null || true
            sleep 1
            pkill -9 -f "$TEMP_CONFIG" 2>/dev/null || true
        fi
        pkill -9 -x dbtest 2>/dev/null || true
        wait "$SHARD_PID" 2>/dev/null || true
        rm -f "$TEMP_CONFIG"
        unset MAKO_CONFIG MAKO_BATCH_SIZE

        # Extract metrics
        throughput=""
        per_core_throughput=""
        avg_latency=""
        avg_persist_latency=""
        abort_rate=""
        avg_cpu=""
        peak_cpu=""
        active_threads=""

        if [ -f "$shard_log" ]; then
            throughput=$(extract_metric "$shard_log" "agg_persist_throughput:")
            per_core_throughput=$(extract_metric "$shard_log" "avg_per_core_persist_throughput:")
            avg_latency=$(extract_metric "$shard_log" "avg_latency:")
            avg_persist_latency=$(extract_metric "$shard_log" "avg_persist_latency:")
            abort_rate=$(extract_metric "$shard_log" "agg_abort_rate:")
        fi

        if [ -f "$cpu_log" ]; then
            avg_cpu=$(awk '!/^#/ && NF>=3 { sum += $2; n++ } END { if (n>0) printf "%.1f", sum/n }' "$cpu_log")
            peak_cpu=$(awk '!/^#/ && NF>=3 { if ($2 > max) max = $2 } END { printf "%.1f", max }' "$cpu_log")
        fi

        if [ -f "$thread_cpu_log" ]; then
            active_threads=$(awk '!/^#/ && NF>=3 { sum += $3; n++ } END { if (n>0) printf "%.0f", sum/n }' "$thread_cpu_log")
        fi

        printf "    Throughput: %s ops/s | CPU: %s%% | Latency: %s ms | ActiveThreads: %s\n" \
            "${throughput:-N/A}" "${avg_cpu:-N/A}" "${avg_latency:-N/A}" "${active_threads:-N/A}"

        echo "${trd},${run},${throughput},${per_core_throughput},${avg_cpu},${peak_cpu},${avg_latency},${avg_persist_latency},${abort_rate},${active_threads},${exit_code}" >> "$CSV_FILE"

        if [ -n "$throughput" ]; then
            thread_throughputs[$trd]="${thread_throughputs[$trd]} $throughput"
        fi

        sleep 2
    done
    echo ""
done

# ============================================================
#  Summary report
# ============================================================
{
echo "============================================================"
echo "  MAKO NO-REPLICATION SCALABILITY SWEEP REPORT"
echo "============================================================"
echo ""
echo "  Date:           $(date)"
echo "  Configuration:  1 shard, NO replication, TPC-C, batch_size=$BATCH_SIZE"
echo "  Transport:      ${MAKO_TRANSPORT:-rrr/rpc}"
echo "  Host:           $(hostname) ($(nproc) cores)"
echo "  Threads:        $THREADS"
echo "  Runs/config:    $NUM_RUNS"
echo "  Git branch:     $(git -C "$SCRIPT_DIR" rev-parse --abbrev-ref HEAD 2>/dev/null || echo 'unknown')"
echo "  Git commit:     $(git -C "$SCRIPT_DIR" rev-parse --short HEAD 2>/dev/null || echo 'unknown')"
echo ""

echo "============================================================"
echo "  SCALABILITY TABLE"
echo "============================================================"
echo ""
printf "  %-8s  %-16s  %-10s  %-10s  %-12s  %-10s\n" \
    "Threads" "Throughput" "Stdev" "CV%" "AvgCPU%" "ScaleEff%"
printf "  %-8s  %-16s  %-10s  %-10s  %-12s  %-10s\n" \
    "-------" "----------" "-----" "---" "-------" "---------"

baseline_throughput=""
baseline_threads=""

for trd in $THREADS; do
    values="${thread_throughputs[$trd]}"
    if [ -z "$values" ]; then
        printf "  %-8s  %-16s  %-10s  %-10s  %-12s  %-10s\n" "$trd" "N/A" "N/A" "N/A" "N/A" "N/A"
        continue
    fi

    stats=$(echo "$values" | tr ' ' '\n' | grep -v '^$' | awk '
    BEGIN { sum=0; n=0 }
    { n++; sum+=$1; vals[n]=$1 }
    END {
        mean = sum/n
        sumsq = 0
        for (i=1; i<=n; i++) sumsq += (vals[i]-mean)^2
        stdev = (n>1) ? sqrt(sumsq/(n-1)) : 0
        cv = (mean>0) ? (stdev/mean)*100 : 0
        printf "%.0f %.0f %.1f", mean, stdev, cv
    }')

    mean=$(echo "$stats" | awk '{print $1}')
    stdev=$(echo "$stats" | awk '{print $2}')
    cv=$(echo "$stats" | awk '{print $3}')

    avg_cpu_for_trd=$(awk -F',' -v t="$trd" '
        NR>1 && $1==t && $5!="" { sum+=$5; n++ }
        END { if (n>0) printf "%.0f", sum/n; else printf "N/A" }
    ' "$CSV_FILE")

    if [ -z "$baseline_throughput" ]; then
        baseline_throughput="$mean"
        baseline_threads="$trd"
    fi

    scale_eff=""
    if [ -n "$baseline_throughput" ] && [ "$baseline_throughput" != "0" ]; then
        scale_eff=$(awk -v actual="$mean" -v base="$baseline_throughput" -v t="$trd" -v bt="$baseline_threads" \
            'BEGIN { printf "%.0f", (actual / (base * (t/bt))) * 100 }')
    fi

    printf "  %-8s  %10s ops/s  %7s  %7s %%  %9s %%  %8s %%\n" \
        "$trd" "$mean" "$stdev" "$cv" "$avg_cpu_for_trd" "${scale_eff:-N/A}"
done

echo ""
echo "============================================================"
echo "  PER-RUN DETAILS"
echo "============================================================"
echo ""
printf "  %-6s  %-4s  %-14s  %-10s  %-10s  %-10s  %-8s\n" \
    "Thrd" "Run" "Throughput" "AvgCPU%" "PeakCPU%" "Latency" "Status"
printf "  %-6s  %-4s  %-14s  %-10s  %-10s  %-10s  %-8s\n" \
    "----" "---" "----------" "-------" "--------" "-------" "------"

tail -n +2 "$CSV_FILE" | while IFS=',' read -r thrd rn tp pct cpu pcpu lat plat ar at ec; do
    status="PASS"
    [ -z "$tp" ] && status="FAIL"
    printf "  %-6s  %-4s  %10s ops/s  %7s %%  %8s %%  %7s ms  %-8s\n" \
        "$thrd" "$rn" "${tp:-N/A}" "${cpu:-N/A}" "${pcpu:-N/A}" "${lat:-N/A}" "$status"
done

echo ""
echo "============================================================"
echo "  FILES"
echo "============================================================"
echo ""
echo "  Results dir:      $RESULTS_DIR"
echo "  Latest link:      $LATEST_LINK"
echo "  Summary:          $SUMMARY_FILE"
echo "  CSV data:         $CSV_FILE"
echo "  Per-run logs:     ${LOGS_DIR}/t*_run*_shard.log"
echo "  CPU logs:         ${LOGS_DIR}/t*_run*_cpu.log"
echo "  Thread CPU logs:  ${LOGS_DIR}/t*_run*_cpu_threads.log"

} 2>&1 | tee "$SUMMARY_FILE"

echo ""
echo "Done. Summary saved to: $SUMMARY_FILE"
echo "CSV data saved to:      $CSV_FILE"

# Auto-generate throughput plot
PLOT_FILE="${RESULTS_DIR}/throughput_vs_threads.png"
python3 "${SCRIPT_DIR}/scripts/plot_no_replication_throughput.py" \
    "$CSV_FILE" \
    --title "Mako Single-Shard, No Replication: Throughput vs Threads" \
    -o "$PLOT_FILE" 2>&1 | sed 's/^/  /' || echo "  (plot generation failed)"
echo "Plot:                   $PLOT_FILE"
