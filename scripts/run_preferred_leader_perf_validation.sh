#!/usr/bin/env bash
# Run a validation-only no-disk sweep after preferred-leader changes.
#
# This intentionally does not replace thesis result snapshots. It copies the
# fresh Single-Raft 1:1 and Multi-Raft CSV/log outputs into a timestamped
# validation directory so they can be compared against the existing thesis data.

set -euo pipefail

REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$REPO"

THREADS="${THREADS:-1 2 3 4 5 6 7 8 9 10 11}"
RUNS="${RUNS:-1}"
BATCH_SIZE="${BATCH_SIZE:-400}"
STAMP="$(date +%Y%m%d_%H%M%S)"
TOP="results/benchmarks/preferred-leader-validation"
OUT="$TOP/$STAMP"

mkdir -p "$OUT"

cat > "$OUT/metadata.txt" <<EOF
kind=preferred-leader-performance-validation
started=$(date -Is)
threads=$THREADS
runs=$RUNS
batch_size=$BATCH_SIZE
single_build=build
multi_build=build_multi
git_branch=$(git rev-parse --abbrev-ref HEAD 2>/dev/null || echo unknown)
git_commit=$(git rev-parse --short HEAD 2>/dev/null || echo unknown)
EOF

echo "===================================================================="
echo "  Preferred-leader performance validation"
echo "  Started:    $(date)"
echo "  Output:     $OUT"
echo "  Threads:    $THREADS"
echo "  Runs/point: $RUNS"
echo "===================================================================="

echo
echo "===================================================================="
echo "  [1/4] Build Single-Raft binary"
echo "===================================================================="
cmake --build build --target dbtest --parallel 16

echo
echo "===================================================================="
echo "  [2/4] Build Multi-Raft binary"
echo "===================================================================="
cmake --build build_multi --target dbtest --parallel 16

echo
echo "===================================================================="
echo "  [3/4] Single-Raft 1:1 ReplayPool sweep"
echo "===================================================================="
bash scripts/sweep_single_raft_1to1.sh "$THREADS"
SRC_SINGLE="$(ls -td results/benchmarks/raft-single/single_1to1_*/ | head -1)"
mkdir -p "$OUT/single_raft"
cp "$SRC_SINGLE/results.csv" "$OUT/single_raft/results.csv"
cp -r "$SRC_SINGLE/logs" "$OUT/single_raft/logs"
echo "Single-Raft copied from: $SRC_SINGLE"

echo
echo "===================================================================="
echo "  [4/4] Multi-Raft sweep"
echo "===================================================================="
BENCH_ROOT_OVERRIDE="$OUT/raw_multi" \
BUILD_DIR=build_multi \
  bash scripts/run_scalability_sweep.sh \
    --backend raft-multi \
    --threads "$THREADS" \
    --runs "$RUNS" \
    --batch-size "$BATCH_SIZE"

SRC_MULTI="$(readlink "$OUT/raw_multi/scalability_latest")"
mkdir -p "$OUT/multi_raft"
cp "$SRC_MULTI/results.csv" "$OUT/multi_raft/results.csv"
cp -r "$SRC_MULTI/logs" "$OUT/multi_raft/logs"
echo "Multi-Raft copied from: $SRC_MULTI"

python3 - "$OUT" <<'PY'
import csv
import pathlib
import sys

out = pathlib.Path(sys.argv[1])
summary = out / "summary_throughput_replay.csv"

rows = []
for backend, rel in [
    ("single_raft_1to1", "single_raft/results.csv"),
    ("multi_raft", "multi_raft/results.csv"),
]:
    path = out / rel
    with path.open(newline="") as f:
        for r in csv.DictReader(f):
            threads = r.get("threads", "")
            replay_vals = []
            for k in ("replay_batch_p1", "replay_batch_p2"):
                try:
                    replay_vals.append(float(r.get(k) or 0))
                except ValueError:
                    replay_vals.append(0)
            replay_min = min(replay_vals) if replay_vals else 0
            rows.append({
                "backend": backend,
                "threads": threads,
                "throughput_ops_sec": r.get("throughput_ops_sec", ""),
                "min_follower_replay_ops_sec": f"{replay_min:.0f}",
                "worker_mean_cpu_pct": r.get("role_worker_mean") or r.get("worker_mean_cpu_pct", ""),
                "worker_peak_cpu_pct": r.get("role_worker_peak") or r.get("worker_peak_cpu_pct", ""),
                "exit_code": r.get("exit_code", ""),
            })

with summary.open("w", newline="") as f:
    fieldnames = [
        "backend",
        "threads",
        "throughput_ops_sec",
        "min_follower_replay_ops_sec",
        "worker_mean_cpu_pct",
        "worker_peak_cpu_pct",
        "exit_code",
    ]
    w = csv.DictWriter(f, fieldnames=fieldnames)
    w.writeheader()
    w.writerows(rows)

print(f"Summary CSV: {summary}")
PY

ln -sfn "$STAMP" "$TOP/latest"

echo
echo "===================================================================="
echo "  DONE: preferred-leader validation sweep"
echo "  Finished: $(date)"
echo "  Output:   $OUT"
echo "===================================================================="
