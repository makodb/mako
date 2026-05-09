#!/bin/bash
# Wait for a ReplayPool matrix tmux session, then plot and index the results.

set -euo pipefail

SESSION="${1:-}"
REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$REPO"

if [ -n "$SESSION" ]; then
    echo "Waiting for tmux session: $SESSION"
    while tmux has-session -t "$SESSION" 2>/dev/null; do
        sleep 60
    done
fi

MATRIX_DIR="$(ls -td results/benchmarks/replay_pool_matrix_* 2>/dev/null | head -1)"
if [ -z "$MATRIX_DIR" ]; then
    echo "No replay_pool_matrix result directory found" >&2
    exit 1
fi

echo "Using matrix results: $MATRIX_DIR"
python3 scripts/plot_replay_pool_matrix.py \
  --csv "$MATRIX_DIR/summary.csv" \
  --outdir doc/thesis/figures/graphs

DEST="results/thesis_results/02_replay_pool_matrix"
mkdir -p "$DEST"
ln -sfn "$REPO/$MATRIX_DIR" "$DEST/raw_run"
ln -sfn "$REPO/$MATRIX_DIR/summary.csv" "$DEST/summary.csv"

echo "Indexed matrix results at: $DEST"
