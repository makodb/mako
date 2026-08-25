#!/bin/bash

# Phase 1: Batch size sweep for PAXOS — find the CPU saturation point.
# Runs shard1Replication (Paxos) once per batch size with CPU monitoring.
# No rebuild needed — batch size is controlled via MAKO_BATCH_SIZE env var.
#
# Usage: bash run_batch_sweep_paxos.sh [batch_sizes...]
#   Default batch sizes: 1 2 4 8 16 32 64 128 256 400
#
# NOTE: This script mirrors run_batch_sweep.sh (Raft) exactly in structure
# and metrics — only the CI command and log filenames differ.

set -o pipefail

# Default batch sizes to sweep (override by passing args)
if [ $# -gt 0 ]; then
    BATCH_SIZES=("$@")
else
    BATCH_SIZES=(1 2 4 8 16 32 64 128 256 400)
fi

TIMESTAMP=$(date +%Y%m%d_%H%M%S)
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BENCH_ROOT="${SCRIPT_DIR}/results/benchmarks/paxos"
RESULTS_DIR="${BENCH_ROOT}/batch_sweep_${TIMESTAMP}"
LOGS_DIR="${RESULTS_DIR}/logs"
SUMMARY_FILE="${RESULTS_DIR}/summary.txt"
CSV_FILE="${RESULTS_DIR}/results.csv"
LATEST_LINK="${BENCH_ROOT}/batch_sweep_latest"

# Paxos test produces: test_1shard_replication.sh_shard0-{node}-6.log
LEADER_LOG="${SCRIPT_DIR}/test_1shard_replication.sh_shard0-localhost-6.log"
FOLLOWER_LOG="${SCRIPT_DIR}/test_1shard_replication.sh_shard0-p1-6.log"

mkdir -p "$LOGS_DIR"
ln -sfn "$RESULTS_DIR" "$LATEST_LINK"

# Helper: extract a numeric value after keyword from log
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

# CPU monitor: samples CPU% of all dbtest processes owned by current user.
# Uses /proc/[pid]/stat — per-core convention: 100% = 1 core fully used.
CPU_MONITOR_PID=""

start_cpu_monitor() {
    local logfile="$1"
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
                    # delta jiffies over 1s with HZ=100: delta directly = %CPU in per-core units
                    # 100% = 1 core fully used, 900% = 9 cores
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
}

stop_cpu_monitor() {
    if [ -n "$CPU_MONITOR_PID" ]; then
        kill $CPU_MONITOR_PID 2>/dev/null || true
        wait $CPU_MONITOR_PID 2>/dev/null || true
        CPU_MONITOR_PID=""
    fi
}

echo "============================================================"
echo "  Mako Paxos Batch Size Sweep (Phase 1)"
echo "  Batch sizes: ${BATCH_SIZES[*]}"
echo "  Started: $(date)"
echo "  Results: $RESULTS_DIR/"
echo "============================================================"
echo ""

# CSV header
echo "batch_size,throughput_ops_sec,avg_cpu_pct,peak_cpu_pct,avg_latency_ms,replay_batch_p1,exit_code" > "$CSV_FILE"

# Print header
printf "  %-10s  %-14s  %-10s  %-10s  %-12s  %-12s  %-6s\n" \
    "BatchSize" "Throughput" "AvgCPU%" "PeakCPU%" "Latency" "Replay(p1)" "Status"
printf "  %-10s  %-14s  %-10s  %-10s  %-12s  %-12s  %-6s\n" \
    "---------" "----------" "-------" "--------" "-------" "----------" "------"

for batch_size in "${BATCH_SIZES[@]}"; do
    # Clean up
    pkill -9 -f "build/dbtest" 2>/dev/null || true
    sleep 5

    # Remove stale logs
    rm -f "$LEADER_LOG" "$FOLLOWER_LOG" 2>/dev/null

    # Start per-process CPU monitoring
    cpu_log="${LOGS_DIR}/batch_${batch_size}_cpu.log"
    start_cpu_monitor "$cpu_log"

    # Run the Paxos test with MAKO_BATCH_SIZE override
    export MAKO_BATCH_SIZE=$batch_size
    bash ./ci/ci.sh shard1Replication > "${LOGS_DIR}/batch_${batch_size}_ci.log" 2>&1
    exit_code=$?
    unset MAKO_BATCH_SIZE

    # Stop CPU monitoring
    stop_cpu_monitor

    # Preserve logs
    cp "$LEADER_LOG" "${LOGS_DIR}/batch_${batch_size}_leader.log" 2>/dev/null || true
    cp "$FOLLOWER_LOG" "${LOGS_DIR}/batch_${batch_size}_follower_p1.log" 2>/dev/null || true

    # Extract metrics
    throughput=""
    avg_latency=""
    replay_p1=""
    avg_cpu=""
    peak_cpu=""

    if [ -f "$LEADER_LOG" ]; then
        throughput=$(extract_metric "$LEADER_LOG" "agg_persist_throughput:")
        avg_latency=$(extract_metric "$LEADER_LOG" "avg_latency:")
    fi
    replay_p1=$(extract_replay_batch "$FOLLOWER_LOG")

    # Calculate average and peak CPU%
    if [ -f "$cpu_log" ]; then
        avg_cpu=$(awk '!/^#/ && NF>=3 { sum += $2; n++ } END { if (n>0) printf "%.1f", sum/n }' "$cpu_log")
        peak_cpu=$(awk '!/^#/ && NF>=3 { if ($2 > max) max = $2 } END { printf "%.1f", max }' "$cpu_log")
    fi

    # Display
    status="PASS"
    [ -z "$throughput" ] && status="FAIL"

    printf "  %-10s  %10s ops/s  %7s %%  %8s %%  %9s ms  %10s  %-6s\n" \
        "$batch_size" \
        "${throughput:-N/A}" \
        "${avg_cpu:-N/A}" \
        "${peak_cpu:-N/A}" \
        "${avg_latency:-N/A}" \
        "${replay_p1:-N/A}" \
        "$status"

    # CSV row
    echo "${batch_size},${throughput},${avg_cpu},${peak_cpu},${avg_latency},${replay_p1},${exit_code}" >> "$CSV_FILE"

    # Clean up between runs
    pkill -9 -f "build/dbtest" 2>/dev/null || true
    sleep 5
done

echo ""

# Generate summary report
{
echo "============================================================"
echo "  MAKO PAXOS BATCH SIZE SWEEP REPORT"
echo "============================================================"
echo ""
echo "  Date:           $(date)"
echo "  Configuration:  1 shard, 6 threads (warehouses), TPC-C"
echo "  Replication:    Multi-Paxos (4 replicas: 3 voters + 1 learner)"
echo "  Transport:      srpc/rpc (TCP/IP)"
echo "  Host:           $(hostname) ($(nproc) cores)"
echo "  Git branch:     $(git -C "$SCRIPT_DIR" rev-parse --abbrev-ref HEAD 2>/dev/null || echo 'unknown')"
echo "  Git commit:     $(git -C "$SCRIPT_DIR" rev-parse --short HEAD 2>/dev/null || echo 'unknown')"
echo "  Batch sizes:    ${BATCH_SIZES[*]}"
echo ""
echo "  NOTE: MAKO_BATCH_SIZE controls how many transactions are"
echo "  serialized into a single Paxos log entry. Default is 400."
echo "  CPU% is per-core: 100% = 1 core, 900% = 9 cores."
echo ""
echo "============================================================"
echo "  RESULTS"
echo "============================================================"
echo ""
printf "  %-10s  %-14s  %-10s  %-10s  %-12s  %-12s\n" \
    "BatchSize" "Throughput" "AvgCPU%" "PeakCPU%" "Latency" "Replay(p1)"
printf "  %-10s  %-14s  %-10s  %-10s  %-12s  %-12s\n" \
    "---------" "----------" "-------" "--------" "-------" "----------"

tail -n +2 "$CSV_FILE" | while IFS=',' read -r bs tp cpu pcpu lat rp ec; do
    printf "  %-10s  %10s ops/s  %7s %%  %8s %%  %9s ms  %10s\n" \
        "$bs" "${tp:-N/A}" "${cpu:-N/A}" "${pcpu:-N/A}" "${lat:-N/A}" "${rp:-N/A}"
done

echo ""
echo "============================================================"
echo "  FILES"
echo "============================================================"
echo ""
echo "  Results dir:   $RESULTS_DIR"
echo "  Latest link:   $LATEST_LINK"
echo "  Summary:       $SUMMARY_FILE"
echo "  CSV data:      $CSV_FILE"
echo "  Per-batch logs: ${LOGS_DIR}/batch_*_leader.log"
echo "  CPU logs:       ${LOGS_DIR}/batch_*_cpu.log"
} | tee "$SUMMARY_FILE"

echo ""
echo "Done. Summary saved to: $SUMMARY_FILE"
