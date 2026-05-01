#!/bin/bash
# Single-Raft sweep where replay-pool size equals worker-thread count for every
# point: at t=N workers, run with MAKO_REPLAY_THREADS=N replay threads.
#
# Produces a merged results.csv identical in layout to a normal sweep, plus
# copies of each per-t sweep dir, so downstream extractors/plotters just work.

set -e

REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$REPO"

THREADS="${1:-1 2 3 4 5 6 7 8 9 10 11}"
OUT="results/benchmarks/raft-single/single_1to1_$(date +%Y%m%d_%H%M%S)"
mkdir -p "$OUT/logs"

merged="$OUT/results.csv"
# Header stays in sync with run_scalability_sweep.sh's per-run CSV (now
# includes role-bucketed CPU columns) plus the trailing `replay_threads`
# annotation specific to this 1:1 wrapper.
echo "threads,run,throughput_ops_sec,per_core_throughput,avg_cpu_pct,peak_cpu_pct,avg_latency_ms,avg_persist_latency_ms,agg_abort_rate,replay_batch_p1,replay_batch_p2,active_threads,worker_mean_cpu_pct,worker_peak_cpu_pct,role_worker_mean,role_worker_peak,role_replay_mean,role_replay_peak,role_apply_peak,role_other_mean,exit_code,replay_threads" > "$merged"

echo "============================================================"
echo "  Single-Raft 1:1 sweep  (replay_threads = worker_threads)"
echo "  Thread counts: $THREADS"
echo "  Output:        $OUT"
echo "============================================================"

for t in $THREADS; do
    echo
    echo "### t=$t  MAKO_REPLAY_THREADS=$t ###"
    MAKO_REPLAY_THREADS=$t INTER_RUN_SLEEP=30 \
      bash scripts/sweep_raft_single.sh \
        --skip-build --threads "$t" --runs 1 --batch-size 400

    src=$(readlink results/benchmarks/raft-single/scalability_latest)

    # copy log files with the thread suffix (they're already t${t}_ prefixed
    # but unique across the outer sweep since we varied t per run).
    cp "$src/logs/"* "$OUT/logs/"

    # append the per-t row, annotated with the replay-thread count.
    tail -n +2 "$src/results.csv" | while IFS= read -r row; do
        printf "%s,%s\n" "$row" "$t" >> "$merged"
    done

    echo "  → merged row for t=$t"
done

echo
echo "Merged results: $merged"
cat "$merged"
