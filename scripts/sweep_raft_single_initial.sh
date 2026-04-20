#!/bin/bash
# Single-Raft scalability sweep — all partitions share ONE Raft group.
#
# Runs t = 1..24 (configurable) once each by default. Does NOT exit on per-run
# failure — a crash at t=N still lets t=N+1 run. CSV + throughput PNG are
# written to results/benchmarks/raft-single/scalability_latest/.
#
# Usage:
#   bash scripts/sweep_raft_single_initial.sh
#   bash scripts/sweep_raft_single_initial.sh --runs 3
#   bash scripts/sweep_raft_single_initial.sh --threads "1 2 4 8 16 24"
#   bash scripts/sweep_raft_single_initial.sh --build            # force rebuild first
#   INTER_RUN_SLEEP=30 bash scripts/sweep_raft_single_initial.sh # shorter inter-run pause
#
# Prereqs:
#   - Single-Raft binary must be built (mako-raft-single target), or pass
#     --build to rebuild it here. NOTE: the build is a FULL clean+rebuild
#     because CMake caches paxos vs raft flags and they conflict. Expect
#     10-30 minutes for a --build.
#   - config/1leader_2followers/raft_generator.py emits 3-replica raft YAMLs.
#     This script re-runs the generator on each invocation so configs stay
#     in sync with whatever raft_generator.py currently produces.

set -o pipefail
# No `set -e`: we want the sweep to survive per-thread-count failures so one
# hang at t=N doesn't throw away t=1..N-1.

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$REPO_ROOT" || exit 1

THREADS="1 2 3 4 5 6 7 8 9 10 11 12 13 14 15 16 17 18 19 20 21 22 23 24"
RUNS=1
BATCH_SIZE=400
DO_BUILD=0

while [[ $# -gt 0 ]]; do
    case "$1" in
        --threads)    THREADS="$2"; shift 2 ;;
        --runs)       RUNS="$2"; shift 2 ;;
        --batch-size) BATCH_SIZE="$2"; shift 2 ;;
        --build)      DO_BUILD=1; shift ;;
        -h|--help)    sed -n '2,21p' "$0"; exit 0 ;;
        *)            echo "Unknown arg: $1"; exit 1 ;;
    esac
done

# Sanity: the inner sweep script invokes bash/shard_raft.sh → dbtest.
if [ ! -x "build/dbtest" ] && [ "$DO_BUILD" -ne 1 ]; then
    echo "ERROR: build/dbtest not found. Run with --build, or build manually:"
    echo "         make clean && make mako-raft-single -j32"
    exit 1
fi

echo "============================================================"
echo "  Single-Raft scalability sweep"
echo "  Threads:       $THREADS"
echo "  Runs/config:   $RUNS"
echo "  Batch size:    $BATCH_SIZE"
echo "  Inter-run nap: ${INTER_RUN_SLEEP:-90}s (env INTER_RUN_SLEEP overrides)"
echo "  Host:          $(hostname)"
echo "  Started:       $(date)"
echo "============================================================"

# Re-emit raft configs to reflect whatever raft_generator.py currently produces.
# Idempotent — safe to run every time.
echo ">>> Regenerating raft configs..."
(cd config/1leader_2followers && python3 raft_generator.py) || {
    echo "ERROR: raft config generation failed"; exit 1
}

# Quick assertion: generated config should have 3 sites per partition, no learner.
if grep -v '^[[:space:]]*#' config/1leader_2followers/raft1_shardidx0.yml 2>/dev/null | grep -q '"s401\|learner'; then
    echo "ERROR: raft1_shardidx0.yml still contains learner references. raft_generator.py needs updating."
    exit 1
fi

if [ "$DO_BUILD" -eq 1 ]; then
    echo ">>> Building single-raft binary (clean + full build, can take 10-30 min)..."
    # Clean first: CMake caches the Paxos-vs-Raft configuration, so switching
    # builds without `make clean` produces a broken binary.
    make clean 2>&1 | tail -3
    make mako-raft-single -j32 2>&1 | tail -5 || {
        echo "ERROR: build failed"; exit 1
    }
fi

# Hand off to the shared sweep driver. It:
#   - runs every (thread, run) pair independently
#   - does NOT exit on per-run failure (no set -e inside)
#   - writes CSV + summary + throughput_vs_threads.png
#   - raft-single branch in start_replicas() launches 3 replicas (no learner)
echo ">>> Starting sweep..."
bash scripts/run_scalability_sweep.sh \
    --backend raft-single \
    --threads "$THREADS" \
    --runs "$RUNS" \
    --batch-size "$BATCH_SIZE"

INNER_EXIT=$?

LATEST="${REPO_ROOT}/results/benchmarks/raft-single/scalability_latest"
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
