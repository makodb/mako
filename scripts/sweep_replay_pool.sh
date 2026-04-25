#!/bin/bash
# Sweep the ReplayPool size at a fixed worker-thread count.
# For each N in {1, 2, 4, 8, 11}, runs a single 30s test at --threads=11 with
# MAKO_REPLAY_THREADS=$N, and copies the results dir into a per-N folder.
#
# Usage: bash scripts/sweep_replay_pool.sh [threads] [sizes]
#   threads: worker thread count (default 11)
#   sizes:   space-separated replay pool sizes (default "1 2 4 8 11")

set -e

THREADS="${1:-11}"
SIZES="${2:-1 2 4 8 11}"
REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$REPO"

OUT="results/benchmarks/replay_pool_sweep_$(date +%Y%m%d_%H%M%S)"
mkdir -p "$OUT"

echo "============================================================"
echo "  Replay pool sweep"
echo "  Worker threads: $THREADS"
echo "  Pool sizes:     $SIZES"
echo "  Output dir:     $OUT"
echo "============================================================"

summary="$OUT/summary.csv"
echo "replay_threads,worker_threads,throughput_ops_sec,replay_batch_p1,batches_per_sec,sweep_dir" > "$summary"

for n in $SIZES; do
    echo
    echo "### MAKO_REPLAY_THREADS=$n ###"
    MAKO_REPLAY_THREADS=$n INTER_RUN_SLEEP=30 \
      bash scripts/sweep_raft_single.sh \
        --skip-build --threads "$THREADS" --runs 1 --batch-size 400

    sweep_dir=$(readlink results/benchmarks/raft-single/scalability_latest)
    dest="$OUT/n${n}"
    cp -r "$sweep_dir" "$dest"
    echo "  → copied to $dest"

    # pull last non-header row
    row=$(tail -n 1 "$sweep_dir/results.csv")
    # columns: threads,run,throughput_ops_sec,...,replay_batch_p1,replay_batch_p2,...
    thr=$(echo "$row" | cut -d, -f1)
    tput=$(echo "$row" | cut -d, -f3)
    rep=$(echo "$row" | cut -d, -f10)
    bps=$(awk -v r=$rep 'BEGIN { printf "%.1f", r/30.0 }')
    echo "$n,$thr,$tput,$rep,$bps,$dest" >> "$summary"
done

echo
echo "Summary:"
cat "$summary"
echo
echo "All runs in: $OUT"
