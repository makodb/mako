#!/bin/bash
# Overnight four-way no-disk sweep covering Step 1 + Step 2 of the thesis plan.
#
#   Conditions (all at t=1..11):
#     1) Single-Raft, NO replay pool   (MAKO_REPLAY_THREADS=0, build/)
#        — reproduces the historical flatline so we can show what was wrong.
#     2) Single-Raft, replay pool 1:1   (MAKO_REPLAY_THREADS=$t, build/)
#        — the parallel-replay design that fixes it.
#     3) Multi-Raft  with replay pool   (MAKO_REPLAY_THREADS=$t, build_multi/)
#        — the reference architecture (one RaftServer per partition).
#     4) Paxos       with replay pool   (MAKO_REPLAY_THREADS=$t, build_paxos/)
#        — the other reference (multi-Paxos).
#
# All four use the existing no-disk binaries (DISABLE_DISK=ON). Persistence
# numbers come from the separate run_simulated_persistence_sweep.sh.
#
# Total runtime: ~5 hours (4 sweeps × ~75 min + plot generation).
#
# Usage:
#   nohup bash scripts/overnight_four_way.sh > overnight_four_way.log 2>&1 &

set -e

REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$REPO"

THREADS="${THREADS:-1 2 3 4 5 6 7 8 9 10 11}"
STAMP=$(date +%Y%m%d_%H%M%S)
OUT="results/benchmarks/overnight_four_way_$STAMP"
mkdir -p "$OUT"

echo "================================================================="
echo "  Overnight four-way no-disk sweep"
echo "  Started: $(date)"
echo "  Output:  $OUT"
echo "  Threads: $THREADS"
echo "================================================================="

# Pre-flight: confirm binaries exist.
for d in build build_multi build_paxos; do
    if [ ! -x "$REPO/$d/dbtest" ]; then
        echo "ERROR: $d/dbtest missing — rebuild before running."
        exit 1
    fi
done
echo "Binaries verified:"
ls -l build/dbtest build_multi/dbtest build_paxos/dbtest

# ---------------------------------------------------------------------
# Condition 1: Single-Raft, NO pool (MAKO_REPLAY_THREADS=0)
# ---------------------------------------------------------------------
echo
echo "================================================================="
echo "  [1/4] Single-Raft NO replay pool  ($(date))"
echo "================================================================="
NOPOOL_OUT="$OUT/single_raft_no_pool"
mkdir -p "$NOPOOL_OUT/logs"
NOPOOL_CSV="$NOPOOL_OUT/results.csv"
echo "threads,run,throughput_ops_sec,per_core_throughput,avg_cpu_pct,peak_cpu_pct,avg_latency_ms,avg_persist_latency_ms,agg_abort_rate,replay_batch_p1,replay_batch_p2,active_threads,worker_mean_cpu_pct,worker_peak_cpu_pct,role_worker_mean,role_worker_peak,role_replay_mean,role_replay_peak,role_apply_peak,role_other_mean,exit_code,fake_leader_total_bytes,fake_leader_total_writes,fake_cluster_total_bytes,fake_cluster_total_writes,fake_leader_mako_data_bytes,fake_leader_raft_log_bytes,fake_cluster_raft_log_bytes,fake_max_wait_us,replay_threads" > "$NOPOOL_CSV"

for t in $THREADS; do
    echo "--- single-Raft no-pool, t=$t (MAKO_REPLAY_THREADS=0) ---"
    MAKO_REPLAY_THREADS=0 INTER_RUN_SLEEP=30 \
      bash scripts/sweep_raft_single.sh \
        --skip-build --threads "$t" --runs 1 --batch-size 400
    src=$(readlink results/benchmarks/raft-single/scalability_latest)
    cp "$src/logs/"* "$NOPOOL_OUT/logs/" 2>/dev/null || true
    tail -n +2 "$src/results.csv" | while IFS= read -r row; do
        printf "%s,0\n" "$row" >> "$NOPOOL_CSV"
    done
done
echo "Single-Raft NO-pool done at $(date).  CSV: $NOPOOL_CSV"

# ---------------------------------------------------------------------
# Condition 2: Single-Raft, replay pool 1:1
# ---------------------------------------------------------------------
echo
echo "================================================================="
echo "  [2/4] Single-Raft 1:1 replay pool  ($(date))"
echo "================================================================="
bash scripts/sweep_single_raft_1to1.sh "$THREADS"
SRC_SINGLE=$(ls -td results/benchmarks/raft-single/single_1to1_*/ | head -1)
cp "$SRC_SINGLE/results.csv" "$OUT/single_raft_pool.csv"
cp -r "$SRC_SINGLE" "$OUT/single_raft_pool_dir"
echo "Single-Raft 1:1 pool done at $(date).  CSV: $OUT/single_raft_pool.csv"

# ---------------------------------------------------------------------
# Condition 3: Multi-Raft (with default pool)
# ---------------------------------------------------------------------
echo
echo "================================================================="
echo "  [3/4] Multi-Raft sweep  ($(date))"
echo "================================================================="
# Multi-Raft uses MAKO_REPLAY_THREADS=$t per worker count too, so the pool
# isn't the bottleneck and the comparison stays fair.
MULTI_OUT="$OUT/multi_raft"
mkdir -p "$MULTI_OUT/logs"
MULTI_CSV="$MULTI_OUT/results.csv"
echo "threads,run,throughput_ops_sec,per_core_throughput,avg_cpu_pct,peak_cpu_pct,avg_latency_ms,avg_persist_latency_ms,agg_abort_rate,replay_batch_p1,replay_batch_p2,active_threads,worker_mean_cpu_pct,worker_peak_cpu_pct,role_worker_mean,role_worker_peak,role_replay_mean,role_replay_peak,role_apply_peak,role_other_mean,exit_code,fake_leader_total_bytes,fake_leader_total_writes,fake_cluster_total_bytes,fake_cluster_total_writes,fake_leader_mako_data_bytes,fake_leader_raft_log_bytes,fake_cluster_raft_log_bytes,fake_max_wait_us,replay_threads" > "$MULTI_CSV"

for t in $THREADS; do
    echo "--- multi-Raft, t=$t (MAKO_REPLAY_THREADS=$t) ---"
    BUILD_DIR=build_multi MAKO_REPLAY_THREADS=$t INTER_RUN_SLEEP=30 \
      bash scripts/run_scalability_sweep.sh \
        --backend raft-multi --threads "$t" --runs 1 --batch-size 400
    src=$(readlink results/benchmarks/raft-multi/scalability_latest)
    cp "$src/logs/"* "$MULTI_OUT/logs/" 2>/dev/null || true
    tail -n +2 "$src/results.csv" | while IFS= read -r row; do
        printf "%s,%s\n" "$row" "$t" >> "$MULTI_CSV"
    done
done
echo "Multi-Raft done at $(date).  CSV: $MULTI_CSV"

# ---------------------------------------------------------------------
# Condition 4: Paxos (with default pool)
# ---------------------------------------------------------------------
echo
echo "================================================================="
echo "  [4/4] Paxos sweep  ($(date))"
echo "================================================================="
PAXOS_OUT="$OUT/paxos"
mkdir -p "$PAXOS_OUT/logs"
PAXOS_CSV="$PAXOS_OUT/results.csv"
echo "threads,run,throughput_ops_sec,per_core_throughput,avg_cpu_pct,peak_cpu_pct,avg_latency_ms,avg_persist_latency_ms,agg_abort_rate,replay_batch_p1,replay_batch_p2,active_threads,worker_mean_cpu_pct,worker_peak_cpu_pct,role_worker_mean,role_worker_peak,role_replay_mean,role_replay_peak,role_apply_peak,role_other_mean,exit_code,fake_leader_total_bytes,fake_leader_total_writes,fake_cluster_total_bytes,fake_cluster_total_writes,fake_leader_mako_data_bytes,fake_leader_raft_log_bytes,fake_cluster_raft_log_bytes,fake_max_wait_us,replay_threads" > "$PAXOS_CSV"

for t in $THREADS; do
    echo "--- paxos, t=$t (MAKO_REPLAY_THREADS=$t) ---"
    BUILD_DIR=build_paxos MAKO_REPLAY_THREADS=$t INTER_RUN_SLEEP=30 \
      bash scripts/run_scalability_sweep.sh \
        --backend paxos --threads "$t" --runs 1 --batch-size 400
    src=$(readlink results/benchmarks/paxos/scalability_latest)
    cp "$src/logs/"* "$PAXOS_OUT/logs/" 2>/dev/null || true
    tail -n +2 "$src/results.csv" | while IFS= read -r row; do
        printf "%s,%s\n" "$row" "$t" >> "$PAXOS_CSV"
    done
done
echo "Paxos done at $(date).  CSV: $PAXOS_CSV"

# ---------------------------------------------------------------------
# Symlinks for convenience + summary
# ---------------------------------------------------------------------
ln -sfT "$(basename "$OUT")" "results/benchmarks/overnight_four_way_latest"

echo
echo "================================================================="
echo "  ALL FOUR CONDITIONS DONE  ($(date))"
echo "  Output dir: $OUT"
echo "  CSVs:"
echo "    - $NOPOOL_CSV"
echo "    - $OUT/single_raft_pool.csv"
echo "    - $MULTI_CSV"
echo "    - $PAXOS_CSV"
echo "  Plot generation: see scripts/plot_overnight_four_way.py (TBD)"
echo "================================================================="
