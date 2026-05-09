#!/bin/bash
# Sweep ReplayPool size across multiple worker-thread counts.
#
# Usage:
#   THREADS="1 2 3 4 5 6 7 8 9 10 11" \
#   SIZES="0 1 2 4 8 11" \
#   bash scripts/sweep_replay_pool_matrix.sh

set -euo pipefail

THREADS="${THREADS:-1 2 3 4 5 6 7 8 9 10 11}"
SIZES="${SIZES:-0 1 2 4 8 11}"
RUNS="${RUNS:-1}"
BATCH_SIZE="${BATCH_SIZE:-400}"
INTER_RUN_SLEEP="${INTER_RUN_SLEEP:-30}"

REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$REPO"

OUT="results/benchmarks/replay_pool_matrix_$(date +%Y%m%d_%H%M%S)"
mkdir -p "$OUT/raw"

summary="$OUT/summary.csv"
printf "worker_threads,replay_threads,throughput_ops_sec,replay_batch_p1,replay_batches_per_sec,role_worker_mean,role_replay_mean,role_apply_peak,avg_cpu_pct,exit_code,sweep_dir\n" > "$summary"

echo "============================================================"
echo "  ReplayPool matrix sweep"
echo "  Worker threads: $THREADS"
echo "  Pool sizes:     $SIZES"
echo "  Runs/config:    $RUNS"
echo "  Output dir:     $OUT"
echo "============================================================"

for t in $THREADS; do
    for n in $SIZES; do
        echo
        echo "### worker_threads=$t  MAKO_REPLAY_THREADS=$n ###"
        MAKO_REPLAY_THREADS="$n" INTER_RUN_SLEEP="$INTER_RUN_SLEEP" \
          bash scripts/sweep_raft_single.sh \
            --skip-build --threads "$t" --runs "$RUNS" --batch-size "$BATCH_SIZE"

        sweep_dir="$(readlink results/benchmarks/raft-single/scalability_latest)"
        dest="$OUT/raw/t${t}_n${n}"
        cp -a "$sweep_dir/." "$dest/"

        row="$(tail -n 1 "$sweep_dir/results.csv")"
        tput="$(printf "%s\n" "$row" | cut -d, -f3)"
        rep="$(printf "%s\n" "$row" | cut -d, -f10)"
        worker_mean="$(printf "%s\n" "$row" | cut -d, -f15)"
        replay_mean="$(printf "%s\n" "$row" | cut -d, -f17)"
        apply_peak="$(printf "%s\n" "$row" | cut -d, -f19)"
        avg_cpu="$(printf "%s\n" "$row" | cut -d, -f5)"
        exit_code="$(printf "%s\n" "$row" | cut -d, -f21)"
        bps="$(awk -v r="$rep" 'BEGIN { printf "%.1f", r / 30.0 }')"
        printf "%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s\n" \
          "$t" "$n" "$tput" "$rep" "$bps" "$worker_mean" "$replay_mean" \
          "$apply_peak" "$avg_cpu" "$exit_code" "$dest" >> "$summary"
    done
done

echo
echo "Summary: $summary"
echo "All runs in: $OUT"
