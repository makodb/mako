#!/bin/bash
# Paxos scalability sweep without the learner replica.
#
# Runs t = 1..16 (configurable) against the standard paxos binary, one run per
# thread count by default. Does NOT exit on per-run failure — each thread count
# is independent, so a crash at t=N still lets t=N+1 run. CSV + throughput PNG
# are written to results/benchmarks/paxos/scalability_latest/.
#
# Usage:
#   bash scripts/sweep_paxos_no_learner.sh
#   bash scripts/sweep_paxos_no_learner.sh --runs 3
#   bash scripts/sweep_paxos_no_learner.sh --threads "1 2 4 8 16"
#   bash scripts/sweep_paxos_no_learner.sh --build            # force rebuild first
#   INTER_RUN_SLEEP=30 bash scripts/sweep_paxos_no_learner.sh # shorter inter-run pause
#
# Prereqs:
#   - ./build/dbtest must exist (or pass --build to rebuild).
#   - config/1leader_2followers/generator.py has been patched to skip learner.
#     This script re-runs the generator on each invocation, so the YAMLs stay
#     in sync with whatever generator.py currently produces.

set -o pipefail
# No `set -e`: we want the sweep to survive per-thread-count failures so one
# hang at t=N doesn't throw away t=1..N-1.

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$REPO_ROOT" || exit 1

THREADS="1 2 3 4 5 6 7 8 9 10 11 12 13 14 15 16"
RUNS=1
BATCH_SIZE=400
DO_BUILD=0

while [[ $# -gt 0 ]]; do
    case "$1" in
        --threads)    THREADS="$2"; shift 2 ;;
        --runs)       RUNS="$2"; shift 2 ;;
        --batch-size) BATCH_SIZE="$2"; shift 2 ;;
        --build)      DO_BUILD=1; shift ;;
        -h|--help)    sed -n '2,18p' "$0"; exit 0 ;;
        *)            echo "Unknown arg: $1"; exit 1 ;;
    esac
done

# Sanity: the inner sweep script invokes bash/shard.sh → dbtest.
if [ ! -x "build/dbtest" ] && [ "$DO_BUILD" -ne 1 ]; then
    echo "ERROR: build/dbtest not found. Run with --build, or build manually: make -j32"
    exit 1
fi

echo "============================================================"
echo "  Paxos scalability sweep (no learner)"
echo "  Threads:       $THREADS"
echo "  Runs/config:   $RUNS"
echo "  Batch size:    $BATCH_SIZE"
echo "  Inter-run nap: ${INTER_RUN_SLEEP:-90}s (env INTER_RUN_SLEEP overrides)"
echo "  Host:          $(hostname)"
echo "  Started:       $(date)"
echo "============================================================"

# Re-emit paxos configs to reflect whatever generator.py currently produces.
# Idempotent — safe to run every time.
echo ">>> Regenerating paxos configs (no-learner variant)..."
(cd config/1leader_2followers && python3 generator.py) || {
    echo "ERROR: paxos config generation failed"; exit 1
}

# Quick assertion: generated config should have 3 sites per partition, not 4.
# Strip comment lines before checking so the commented example in the template
# ( "# - [\"s101:7001\", ..., \"s401:7301\"]" ) doesn't produce a false positive.
if grep -v '^[[:space:]]*#' config/1leader_2followers/paxos1_shardidx0.yml 2>/dev/null | grep -q '"s401\|learner'; then
    echo "ERROR: paxos1_shardidx0.yml still contains learner references. generator.py needs updating."
    exit 1
fi

if [ "$DO_BUILD" -eq 1 ]; then
    echo ">>> Building mako paxos binary..."
    make -j32 2>&1 | tail -3 || {
        echo "ERROR: build failed"; exit 1
    }
fi

# Hand off to the shared sweep driver. It:
#   - runs every (thread, run) pair independently
#   - does NOT exit on per-run failure (no set -e inside)
#   - writes CSV + summary + throughput_vs_threads.png
#   - paxos branch in start_replicas() no longer launches a learner
echo ">>> Starting sweep..."
bash scripts/run_scalability_sweep.sh \
    --backend paxos \
    --threads "$THREADS" \
    --runs "$RUNS" \
    --batch-size "$BATCH_SIZE"

INNER_EXIT=$?

LATEST="${REPO_ROOT}/results/benchmarks/paxos/scalability_latest"
PLOT="${LATEST}/throughput_vs_threads.png"
CSV="${LATEST}/results.csv"
SUMMARY="${LATEST}/summary.txt"

echo ""
echo "============================================================"
echo "  Sweep finished (inner exit=$INNER_EXIT)"
echo "  CSV:     $CSV"
echo "  Summary: $SUMMARY"
echo "  Plot:    $PLOT"
echo "  Ended:   $(date)"
echo "============================================================"
