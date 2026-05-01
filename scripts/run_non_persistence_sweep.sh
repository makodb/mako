#!/bin/bash
# Overnight driver: run Single-Raft (1:1 replay pool), Multi-Raft, Paxos at
# t=1..11 with persistence OFF, then generate comparison plots.
#
# Uses three pre-built binaries:
#   build/dbtest        — single-Raft, has ReplayPool
#   build_multi/dbtest  — multi-Raft, no pool (pure reference architecture)
#   build_paxos/dbtest  — multi-Paxos, no pool
#
# Total runtime: ~75 minutes (three sweeps × ~23 min each + plot generation).
# Default INTER_RUN_SLEEP=90 so the per-thread CPU monitor has time to sample.
#
# Output layout:
#   results/benchmarks/non-persistence-results/<STAMP>/
#     single_raft/results.csv
#     multi_raft/results.csv
#     paxos/results.csv
#     plots/
#
# Usage:
#   nohup bash scripts/run_non_persistence_sweep.sh > non_persistence.log 2>&1 &

set -e

REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$REPO"

THREADS="${THREADS:-1 2 3 4 5 6 7 8 9 10 11}"
STAMP=$(date +%Y%m%d_%H%M%S)
TOP="results/benchmarks/non-persistence-results"
OUT="$TOP/$STAMP"
mkdir -p "$OUT"

echo "===================================================================="
echo "  Overnight three-way no-persistence sweep"
echo "  Started: $(date)"
echo "  Output:  $OUT"
echo "  Threads: $THREADS"
echo "===================================================================="

# Pre-flight: confirm all three binaries exist.
for d in build build_multi build_paxos; do
    if [ ! -x "$REPO/$d/dbtest" ]; then
        echo "ERROR: $d/dbtest missing — build it before running overnight."
        exit 1
    fi
done
echo "Binaries verified:"
ls -l build/dbtest build_multi/dbtest build_paxos/dbtest

cat > "$OUT/metadata.txt" <<EOF
kind=no-persistence
started=$(date -Is)
threads=$THREADS
single_build=build
multi_build=build_multi
paxos_build=build_paxos
EOF

# ========================================================================
# Sweep 1: Single-Raft with replay_threads = worker_threads  (build/)
# ========================================================================
echo
echo "===================================================================="
echo "  [1/3] Single-Raft 1:1 replay pool sweep  ($(date))"
echo "===================================================================="
bash scripts/sweep_single_raft_1to1.sh "$THREADS"
SRC_SINGLE=$(ls -td results/benchmarks/raft-single/single_1to1_*/ | head -1)
mkdir -p "$OUT/single_raft"
cp "$SRC_SINGLE/results.csv" "$OUT/single_raft/results.csv"
cp -r "$SRC_SINGLE/logs" "$OUT/single_raft/logs"
echo "Single-Raft 1:1 done at $(date).  CSV: $OUT/single_raft/results.csv"

# ========================================================================
# Sweep 2: Multi-Raft  (build_multi/)
# ========================================================================
echo
echo "===================================================================="
echo "  [2/3] Multi-Raft sweep  ($(date))"
echo "===================================================================="
BUILD_DIR=build_multi bash scripts/run_scalability_sweep.sh \
    --backend raft-multi --threads "$THREADS" --runs 1 --batch-size 400
SRC_MULTI=$(readlink results/benchmarks/raft-multi/scalability_latest)
mkdir -p "$OUT/multi_raft"
cp "$SRC_MULTI/results.csv" "$OUT/multi_raft/results.csv"
cp -r "$SRC_MULTI/logs" "$OUT/multi_raft/logs"
echo "Multi-Raft done at $(date).  CSV: $OUT/multi_raft/results.csv"

# ========================================================================
# Sweep 3: Paxos  (build_paxos/)
# ========================================================================
echo
echo "===================================================================="
echo "  [3/3] Paxos sweep  ($(date))"
echo "===================================================================="
BUILD_DIR=build_paxos bash scripts/run_scalability_sweep.sh \
    --backend paxos --threads "$THREADS" --runs 1 --batch-size 400
SRC_PAXOS=$(readlink results/benchmarks/paxos/scalability_latest)
mkdir -p "$OUT/paxos"
cp "$SRC_PAXOS/results.csv" "$OUT/paxos/results.csv"
cp -r "$SRC_PAXOS/logs" "$OUT/paxos/logs"
echo "Paxos done at $(date).  CSV: $OUT/paxos/results.csv"

# ========================================================================
# Plots
# ========================================================================
echo
echo "===================================================================="
echo "  Generating comparison plots  ($(date))"
echo "===================================================================="
mkdir -p "$OUT/plots"
python3 scripts/plot_overnight_three_way.py \
    --single "$OUT/single_raft/results.csv" \
    --multi  "$OUT/multi_raft/results.csv" \
    --paxos  "$OUT/paxos/results.csv" \
    --outdir "$OUT/plots"

ln -sfn "$STAMP" "$TOP/latest"

echo
echo "===================================================================="
echo "  ALL DONE  ($(date))"
echo "  Output directory: $OUT"
echo "  Plots:            $OUT/plots/"
echo "===================================================================="
ls -la "$OUT/plots"
