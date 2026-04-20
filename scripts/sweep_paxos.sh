#!/bin/bash

# Thread-count sweep for Mako with Multi-Paxos replication.
# Rebuilds the Paxos binary, then runs run_scalability_sweep.sh.
#
# Usage:
#   bash scripts/sweep_paxos.sh
#   bash scripts/sweep_paxos.sh --threads "1 2 4 8 16 24" --runs 3 --batch-size 400
#   bash scripts/sweep_paxos.sh --skip-build        # reuse existing binary

set -e

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$REPO_ROOT" || exit 1

THREADS="1 2 4 8 16 24"
RUNS=3
BATCH_SIZE=400
SKIP_BUILD=0

while [[ $# -gt 0 ]]; do
    case "$1" in
        --threads)    THREADS="$2"; shift 2 ;;
        --runs)       RUNS="$2"; shift 2 ;;
        --batch-size) BATCH_SIZE="$2"; shift 2 ;;
        --skip-build) SKIP_BUILD=1; shift ;;
        -h|--help)    sed -n '3,8p' "$0"; exit 0 ;;
        *)            echo "Unknown arg: $1"; exit 1 ;;
    esac
done

echo "============================================================"
echo "  Mako Paxos Thread Sweep"
echo "  Threads:    $THREADS"
echo "  Runs/conf:  $RUNS"
echo "  Batch size: $BATCH_SIZE"
echo "============================================================"

if [ "$SKIP_BUILD" -eq 0 ]; then
    echo ">>> Building Paxos binary..."
    # Use cmake directly with --target dbtest to skip the third-party eRPC demo
    # binaries (hello_world, latency). Those have an intermittent LTO "Bus
    # error" under -j32 on this toolchain and failing them aborts the whole
    # make target. -j16 is a further safety against transient LTO flakes.
    rm -rf build
    cmake -S . -B build -DMAKO_USE_RAFT=OFF
    cmake --build build --target dbtest --parallel 16
fi

bash scripts/run_scalability_sweep.sh \
    --backend paxos \
    --threads "$THREADS" \
    --runs "$RUNS" \
    --batch-size "$BATCH_SIZE"

echo ""
echo "Results: results/benchmarks/paxos/scalability_latest/"
