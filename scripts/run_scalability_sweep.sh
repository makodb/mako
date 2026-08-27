#!/bin/bash

# Thesis Scalability Sweep — varies worker thread count across backends.
# Produces throughput + CPU data for the three-act story:
#   Act 1: Paxos baseline (linear scaling, workers at ~100% CPU)
#   Act 2: Multi-Raft (same throughput as Paxos = valid drop-in)
#   Act 3: Single-Raft (same throughput, less total CPU)
#
# Usage:
#   bash run_scalability_sweep.sh --backend paxos --threads "1 2 4 6 8 12 16" --runs 3
#   bash run_scalability_sweep.sh --backend raft-single --threads "1 2 4 6 8 12 16" --runs 3
#   bash run_scalability_sweep.sh --backend raft-multi --threads "1 2 4 6 8 12 16" --runs 3
#
# Prerequisites:
#   - Binary must be pre-built for the correct backend:
#       Paxos:        make -j32
#       Raft-single:  make clean && make mako-raft-single -j32
#       Raft-multi:   make clean && make mako-raft-multi -j32
#   - Raft configs generated: cd config/1leader_2followers && python3 raft_generator.py
#   - MAKO_BATCH_SIZE override compiled in (see Transaction.cc)

set -o pipefail

# ============================================================
#  Parse arguments
# ============================================================
BACKEND=""
THREADS="1 2 4 6 8 12 16"
BATCH_SIZE=400
NUM_RUNS=3

while [[ $# -gt 0 ]]; do
    case "$1" in
        --backend)    BACKEND="$2"; shift 2 ;;
        --threads)    THREADS="$2"; shift 2 ;;
        --batch-size) BATCH_SIZE="$2"; shift 2 ;;
        --runs)       NUM_RUNS="$2"; shift 2 ;;
        *)            echo "Unknown arg: $1"; exit 1 ;;
    esac
done

if [ -z "$BACKEND" ]; then
    echo "Error: --backend required (paxos|raft-single|raft-multi)"
    exit 1
fi

case "$BACKEND" in
    paxos|raft-single|raft-multi) ;;
    *) echo "Error: --backend must be paxos, raft-single, or raft-multi"; exit 1 ;;
esac

# ============================================================
#  Setup directories
# ============================================================
TIMESTAMP=$(date +%Y%m%d_%H%M%S)
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BENCH_ROOT="${SCRIPT_DIR}/results/benchmarks/${BACKEND}"
RESULTS_DIR="${BENCH_ROOT}/scalability_${TIMESTAMP}"
LOGS_DIR="${RESULTS_DIR}/logs"
SUMMARY_FILE="${RESULTS_DIR}/summary.txt"
CSV_FILE="${RESULTS_DIR}/results.csv"
LATEST_LINK="${BENCH_ROOT}/scalability_latest"

mkdir -p "$LOGS_DIR"
ln -sfn "$RESULTS_DIR" "$LATEST_LINK"

# ============================================================
#  Helpers (reused from existing benchmark scripts)
# ============================================================

# Extract a numeric value from a log line matching a keyword.
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

extract_replay_batch() {
    local file="$1"
    if [ -f "$file" ]; then
        grep "replay_batch:" "$file" | tail -1 | sed -n 's/.*replay_batch:\([0-9]*\).*/\1/p'
    fi
}

# ============================================================
#  CPU Monitor (process-level + per-thread)
# ============================================================
CPU_MONITOR_PID=""
THREAD_MONITOR_PID=""
NUM_CPUS=$(nproc)

start_cpu_monitor() {
    local logfile="$1"
    local thread_logfile="$2"

    # Process-level CPU monitor
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

    # Per-thread CPU monitor (proves each worker thread is at ~100%)
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
                    if [ "$delta" -gt 5 ]; then  # Only show threads using >5% CPU
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
#  Start replicas based on backend
# ============================================================
start_replicas() {
    local trd="$1"
    local leader_log="$2"
    local follower_p1_log="$3"
    local follower_p2_log="$4"
    local learner_log="$5"

    export MAKO_BATCH_SIZE=$BATCH_SIZE

    case "$BACKEND" in
        paxos)
            nohup bash bash/shard.sh 1 0 "$trd" localhost 0 1 paxos > "$leader_log" 2>&1 &
            nohup bash bash/shard.sh 1 0 "$trd" p2 0 1 paxos > "$follower_p2_log" 2>&1 &
            sleep 1
            nohup bash bash/shard.sh 1 0 "$trd" p1 0 1 paxos > "$follower_p1_log" 2>&1 &
            sleep 1
            nohup bash bash/shard.sh 1 0 "$trd" learner 0 1 paxos > "$learner_log" 2>&1 &
            ;;
        raft-single|raft-multi)
            nohup bash bash/shard_raft.sh 1 0 "$trd" localhost 0 1 > "$leader_log" 2>&1 &
            nohup bash bash/shard_raft.sh 1 0 "$trd" p2 0 1 > "$follower_p2_log" 2>&1 &
            sleep 1
            nohup bash bash/shard_raft.sh 1 0 "$trd" p1 0 1 > "$follower_p1_log" 2>&1 &
            ;;
    esac

    unset MAKO_BATCH_SIZE
}

# ============================================================
#  Main sweep
# ============================================================
echo "============================================================"
echo "  Mako Scalability Sweep"
echo "  Backend:     $BACKEND"
echo "  Threads:     $THREADS"
echo "  Batch size:  $BATCH_SIZE"
echo "  Runs/config: $NUM_RUNS"
echo "  Started:     $(date)"
echo "  Results:     $RESULTS_DIR/"
echo "  Git branch:  $(git -C "$SCRIPT_DIR" rev-parse --abbrev-ref HEAD 2>/dev/null || echo 'unknown')"
echo "  Git commit:  $(git -C "$SCRIPT_DIR" rev-parse --short HEAD 2>/dev/null || echo 'unknown')"
echo "============================================================"
echo ""

# CSV header
echo "threads,run,throughput_ops_sec,per_core_throughput,avg_cpu_pct,peak_cpu_pct,avg_latency_ms,avg_persist_latency_ms,agg_abort_rate,replay_batch_p1,replay_batch_p2,active_threads,exit_code" > "$CSV_FILE"

# Accumulate per-thread-count means for final summary
declare -A thread_throughputs  # thread_count -> space-separated throughputs

for trd in $THREADS; do
    echo "============================================================"
    echo "  Thread count: $trd"
    echo "============================================================"

    for run in $(seq 1 "$NUM_RUNS"); do
        echo "  --- Run $run / $NUM_RUNS (threads=$trd) ---"

        # Cleanup
        pkill -9 -f "build/dbtest" 2>/dev/null || true
        USERNAME=${USER:-unknown}
        rm -rf /tmp/${USERNAME}_mako_rocksdb_shard* 2>/dev/null
        sleep 5

        # Log file paths
        leader_log="${LOGS_DIR}/t${trd}_run${run}_leader.log"
        follower_p1_log="${LOGS_DIR}/t${trd}_run${run}_follower_p1.log"
        follower_p2_log="${LOGS_DIR}/t${trd}_run${run}_follower_p2.log"
        learner_log="${LOGS_DIR}/t${trd}_run${run}_learner.log"
        cpu_log="${LOGS_DIR}/t${trd}_run${run}_cpu.log"
        thread_cpu_log="${LOGS_DIR}/t${trd}_run${run}_cpu_threads.log"

        # Start CPU monitoring
        start_cpu_monitor "$cpu_log" "$thread_cpu_log"

        # Start replicas
        start_replicas "$trd" "$leader_log" "$follower_p1_log" "$follower_p2_log" "$learner_log"
        sleep 2

        # Poll for completion (timeout: 180s for high thread counts)
        max_wait=180
        wait_count=0
        while [ $wait_count -lt $max_wait ]; do
            if [ -f "$leader_log" ] && grep -q "agg_persist_throughput" "$leader_log" 2>/dev/null; then
                echo "    Benchmark completed after ${wait_count}s"
                sleep 2
                break
            fi
            sleep 1
            wait_count=$((wait_count + 1))
            if [ $((wait_count % 30)) -eq 0 ]; then
                echo "    ... waiting (${wait_count}s elapsed)"
            fi
        done

        exit_code=0
        if [ $wait_count -ge $max_wait ]; then
            echo "    WARNING: Timeout after ${max_wait}s"
            exit_code=1
        fi

        # Stop CPU monitoring
        stop_cpu_monitor

        # Graceful shutdown
        pkill -TERM -f "dbtest" 2>/dev/null || true
        sleep 3
        pkill -9 -f "build/dbtest" 2>/dev/null || true
        sleep 1

        # Extract metrics
        throughput=""
        per_core_throughput=""
        avg_latency=""
        avg_persist_latency=""
        abort_rate=""
        replay_p1=""
        replay_p2=""
        avg_cpu=""
        peak_cpu=""
        active_threads=""

        if [ -f "$leader_log" ]; then
            throughput=$(extract_metric "$leader_log" "agg_persist_throughput:")
            per_core_throughput=$(extract_metric "$leader_log" "avg_per_core_persist_throughput:")
            avg_latency=$(extract_metric "$leader_log" "avg_latency:")
            avg_persist_latency=$(extract_metric "$leader_log" "avg_persist_latency:")
            abort_rate=$(extract_metric "$leader_log" "agg_abort_rate:")
        fi
        replay_p1=$(extract_replay_batch "$follower_p1_log")
        replay_p2=$(extract_replay_batch "$follower_p2_log")

        # CPU stats from process monitor
        if [ -f "$cpu_log" ]; then
            avg_cpu=$(awk '!/^#/ && NF>=3 { sum += $2; n++ } END { if (n>0) printf "%.1f", sum/n }' "$cpu_log")
            peak_cpu=$(awk '!/^#/ && NF>=3 { if ($2 > max) max = $2 } END { printf "%.1f", max }' "$cpu_log")
        fi

        # Active threads from per-thread monitor (threads using >5% CPU)
        if [ -f "$thread_cpu_log" ]; then
            active_threads=$(awk '!/^#/ && NF>=3 { sum += $3; n++ } END { if (n>0) printf "%.0f", sum/n }' "$thread_cpu_log")
        fi

        # Display
        printf "    Throughput: %s ops/s | CPU: %s%% | Latency: %s ms | ActiveThreads: %s\n" \
            "${throughput:-N/A}" "${avg_cpu:-N/A}" "${avg_latency:-N/A}" "${active_threads:-N/A}"

        # CSV row
        echo "${trd},${run},${throughput},${per_core_throughput},${avg_cpu},${peak_cpu},${avg_latency},${avg_persist_latency},${abort_rate},${replay_p1},${replay_p2},${active_threads},${exit_code}" >> "$CSV_FILE"

        # Accumulate for summary
        if [ -n "$throughput" ]; then
            thread_throughputs[$trd]="${thread_throughputs[$trd]} $throughput"
        fi

        # Cleanup between runs
        pkill -9 -f "build/dbtest" 2>/dev/null || true
        sleep 3
    done
    echo ""
done

# ============================================================
#  Summary report
# ============================================================
{
echo "============================================================"
echo "  MAKO SCALABILITY SWEEP REPORT"
echo "============================================================"
echo ""
echo "  Date:           $(date)"
echo "  Backend:        $BACKEND"
echo "  Configuration:  1 shard, TPC-C, batch_size=$BATCH_SIZE"
case "$BACKEND" in
    paxos)       echo "  Replication:    Multi-Paxos (4 replicas: 3 voters + 1 learner)" ;;
    raft-single) echo "  Replication:    Raft single-instance (3 replicas, 1 Raft group for all partitions)" ;;
    raft-multi)  echo "  Replication:    Raft multi-instance (3 replicas, 1 Raft group per partition)" ;;
esac
echo "  Transport:      srpc/rpc (TCP/IP)"
echo "  Host:           $(hostname)"
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

# Compute baseline (1-thread throughput) for scaling efficiency
baseline_throughput=""

for trd in $THREADS; do
    values="${thread_throughputs[$trd]}"
    if [ -z "$values" ]; then
        printf "  %-8s  %-16s  %-10s  %-10s  %-12s  %-10s\n" "$trd" "N/A" "N/A" "N/A" "N/A" "N/A"
        continue
    fi

    # Compute mean, stdev, CV from accumulated throughputs
    stats=$(echo "$values" | tr ' ' '\n' | grep -v '^$' | awk -v trd="$trd" '
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

    # Compute avg CPU% for this thread count
    avg_cpu_for_trd=$(awk -F',' -v t="$trd" '
        NR>1 && $1==t && $5!="" { sum+=$5; n++ }
        END { if (n>0) printf "%.0f", sum/n; else printf "N/A" }
    ' "$CSV_FILE")

    # Set baseline from first thread count
    if [ -z "$baseline_throughput" ]; then
        baseline_throughput="$mean"
    fi

    # Scaling efficiency
    scale_eff=""
    if [ -n "$baseline_throughput" ] && [ "$baseline_throughput" != "0" ]; then
        scale_eff=$(awk -v actual="$mean" -v base="$baseline_throughput" -v t="$trd" \
            'BEGIN { printf "%.0f", (actual / (base * t)) * 100 }')
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

tail -n +2 "$CSV_FILE" | while IFS=',' read -r thrd rn tp pct cpu pcpu lat plat ar rp1 rp2 at ec; do
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
echo "  Per-run logs:     ${LOGS_DIR}/t*_run*_leader.log"
echo "  CPU logs:         ${LOGS_DIR}/t*_run*_cpu.log"
echo "  Thread CPU logs:  ${LOGS_DIR}/t*_run*_cpu_threads.log"

} 2>&1 | tee "$SUMMARY_FILE"

echo ""
echo "Done. Summary saved to: $SUMMARY_FILE"
echo "CSV data saved to:      $CSV_FILE"
