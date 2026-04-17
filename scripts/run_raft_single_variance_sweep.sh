#!/bin/bash
# Detached launcher for the Single-Raft 5-run variance sweep.
#
# Runs run_scalability_sweep.sh in the background (survives logout via nohup +
# disown), writes a timestamped launcher log, and prints the PID + commands to
# monitor progress.
#
# Usage:
#   bash scripts/run_raft_single_variance_sweep.sh
#   bash scripts/run_raft_single_variance_sweep.sh --threads "1 2 4 6 8" --runs 3

set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$REPO_ROOT" || exit 1

THREADS="1 2 4 6 8 12 16"
RUNS=2
BATCH_SIZE=400
BACKEND="raft-single"

while [[ $# -gt 0 ]]; do
    case "$1" in
        --threads)    THREADS="$2"; shift 2 ;;
        --runs)       RUNS="$2"; shift 2 ;;
        --batch-size) BATCH_SIZE="$2"; shift 2 ;;
        --backend)    BACKEND="$2"; shift 2 ;;
        *)            echo "Unknown arg: $1"; exit 1 ;;
    esac
done

TIMESTAMP="$(date +%Y%m%d_%H%M%S)"
LOG_DIR="${REPO_ROOT}/results/launcher_logs"
LOG_FILE="${LOG_DIR}/${BACKEND}_variance_${TIMESTAMP}.log"
PID_FILE="${LOG_DIR}/${BACKEND}_variance_${TIMESTAMP}.pid"
mkdir -p "$LOG_DIR"

echo "Launching ${BACKEND} variance sweep"
echo "  threads:    ${THREADS}"
echo "  runs:       ${RUNS}"
echo "  batch size: ${BATCH_SIZE}"
echo "  log file:   ${LOG_FILE}"

nohup bash scripts/run_scalability_sweep.sh \
    --backend "$BACKEND" \
    --threads "$THREADS" \
    --runs "$RUNS" \
    --batch-size "$BATCH_SIZE" \
    > "$LOG_FILE" 2>&1 &

PID=$!
disown "$PID" 2>/dev/null || true
echo "$PID" > "$PID_FILE"

echo ""
echo "  PID:        ${PID}  (saved to ${PID_FILE})"
echo ""
echo "Monitor progress:"
echo "  tail -f ${LOG_FILE}"
echo ""
echo "Check if still running:"
echo "  ps -p ${PID} -o pid,etime,cmd"
echo ""
echo "Kill if needed:"
echo "  kill \$(cat ${PID_FILE}); pkill -f run_scalability_sweep; pkill -f dbtest"
echo ""
echo "When done, plot variance:"
echo "  python3 scripts/plot_variance.py \\"
echo "      --csv results/benchmarks/${BACKEND}/scalability_latest/results.csv \\"
echo "      --out results/variance_${BACKEND}.png \\"
echo "      --label \"${BACKEND}\""
