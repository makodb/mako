#!/bin/bash
set -euo pipefail

cd /home/users/mmakadia/mako

WAIT_FOR_TMUX_SESSION="${WAIT_FOR_TMUX_SESSION:-mako_proof_sweep_20260504_230526}"
THREADS_FULL="${THREADS_FULL:-1 2 3 4 5 6 7 8 9 10 11}"
REPLAY_SENSITIVITY_THREADS="${REPLAY_SENSITIVITY_THREADS:-11}"
REPLAY_SENSITIVITY_SIZES="${REPLAY_SENSITIVITY_SIZES:-0 1 2 4 8 11}"
RUN_VARIANCE="${RUN_VARIANCE:-0}"
STAMP="${STAMP:-$(date +%Y%m%d_%H%M%S)}"
LOG_ROOT="sweep_logs"
mkdir -p "$LOG_ROOT"

export MPLCONFIGDIR="${MPLCONFIGDIR:-/home/users/mmakadia/.local/tmp/matplotlib}"
mkdir -p "$MPLCONFIGDIR"

echo "============================================================"
echo "  Thesis follow-up sweeps"
echo "  Started: $(date)"
echo "  Waiting for tmux session: $WAIT_FOR_TMUX_SESSION"
echo "  Full threads: $THREADS_FULL"
echo "  Replay sensitivity: t=$REPLAY_SENSITIVITY_THREADS sizes=$REPLAY_SENSITIVITY_SIZES"
echo "  Run variance: $RUN_VARIANCE"
echo "============================================================"

if command -v tmux >/dev/null 2>&1 && [ -n "$WAIT_FOR_TMUX_SESSION" ]; then
  while tmux has-session -t "$WAIT_FOR_TMUX_SESSION" 2>/dev/null; do
    echo "[$(date)] Waiting for $WAIT_FOR_TMUX_SESSION to finish..."
    sleep 60
  done
fi

echo
echo ">>> Running four-way no-disk bottleneck/fix sweep"
THREADS="$THREADS_FULL" bash scripts/overnight_four_way.sh
FOUR_WAY_DIR="$(ls -td results/benchmarks/overnight_four_way_* | head -1)"
echo "Four-way results: $FOUR_WAY_DIR"

echo
echo ">>> Running ReplayPool sensitivity sweep"
bash scripts/sweep_replay_pool.sh "$REPLAY_SENSITIVITY_THREADS" "$REPLAY_SENSITIVITY_SIZES"
REPLAY_POOL_DIR="$(ls -td results/benchmarks/replay_pool_sweep_* | head -1)"
echo "ReplayPool sensitivity results: $REPLAY_POOL_DIR"

echo
echo ">>> Plotting ReplayPool sensitivity"
python3 scripts/plot_replay_pool_sensitivity.py \
  --csv "$REPLAY_POOL_DIR/summary.csv" \
  --out "$REPLAY_POOL_DIR/replay_pool_sensitivity.png"

if [ "$RUN_VARIANCE" = "1" ]; then
  echo
  echo ">>> Running headline variance points"
  OUT="results/benchmarks/thesis_headline_variance_$STAMP"
  mkdir -p "$OUT"

  MAKO_REPLAY_THREADS=11 INTER_RUN_SLEEP=30 \
    bash scripts/run_scalability_sweep.sh \
      --backend raft-single --threads "11" --runs 3 --batch-size 400
  cp -r "$(readlink results/benchmarks/raft-single/scalability_latest)" "$OUT/single_raft_t11_runs3"

  BUILD_DIR=build_multi MAKO_REPLAY_THREADS=11 INTER_RUN_SLEEP=30 \
    bash scripts/run_scalability_sweep.sh \
      --backend raft-multi --threads "11" --runs 3 --batch-size 400
  cp -r "$(readlink results/benchmarks/raft-multi/scalability_latest)" "$OUT/multi_raft_t11_runs3"

  BUILD_DIR=build_paxos MAKO_REPLAY_THREADS=11 INTER_RUN_SLEEP=30 \
    bash scripts/run_scalability_sweep.sh \
      --backend paxos --threads "11" --runs 3 --batch-size 400
  cp -r "$(readlink results/benchmarks/paxos/scalability_latest)" "$OUT/paxos_t11_runs3"

  THREADS="11" DISK_LABEL=cloudssd MAKO_PERSIST_BW_MBPS=1000 MAKO_PERSIST_LATENCY_US=1000 \
    bash scripts/run_simulated_persistence_sweep.sh
  cp -r "$(readlink results/benchmarks/simulated-persistence-results/latest)" "$OUT/cloudssd_t11"

  echo "Headline variance results: $OUT"
fi

cat > "results/benchmarks/disk_compare_replay/thesis_followup_latest_paths_${STAMP}.txt" <<EOF
four_way=$FOUR_WAY_DIR
replay_pool=$REPLAY_POOL_DIR
run_variance=$RUN_VARIANCE
EOF
cp "results/benchmarks/disk_compare_replay/thesis_followup_latest_paths_${STAMP}.txt" \
   "results/benchmarks/disk_compare_replay/thesis_followup_latest_paths.txt"

echo
echo "============================================================"
echo "  Thesis follow-up sweeps finished: $(date)"
echo "  Four-way:    $FOUR_WAY_DIR"
echo "  ReplayPool:  $REPLAY_POOL_DIR"
echo "============================================================"
