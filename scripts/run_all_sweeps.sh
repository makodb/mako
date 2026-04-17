#!/bin/bash

# Run scalability sweeps for all three replication backends in one go.
# Rebuilds the binary for each backend before running.
#
# Usage:
#   bash run_all_sweeps.sh
#   bash run_all_sweeps.sh --threads "1 2 4 8 16 24"
#   bash run_all_sweeps.sh --runs 3
#   bash run_all_sweeps.sh --backends "paxos raft-single"

set -e

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$REPO_ROOT" || exit 1

THREADS="1 2 4 6 8 12 16"
RUNS=1
BACKENDS="paxos raft-single raft-multi"

while [[ $# -gt 0 ]]; do
    case "$1" in
        --threads)  THREADS="$2"; shift 2 ;;
        --runs)     RUNS="$2"; shift 2 ;;
        --backends) BACKENDS="$2"; shift 2 ;;
        *)          echo "Unknown arg: $1"; exit 1 ;;
    esac
done

echo "============================================================"
echo "  Mako Full Scalability Sweep"
echo "  Backends:  $BACKENDS"
echo "  Threads:   $THREADS"
echo "  Runs:      $RUNS"
echo "  Started:   $(date)"
echo "============================================================"
echo ""

FAILED_BACKENDS=""
SUCCEEDED_BACKENDS=""

for backend in $BACKENDS; do
    echo ""
    echo "############################################################"
    echo "  Building and running: $backend"
    echo "############################################################"
    echo ""

    # Build the correct binary
    case "$backend" in
        paxos)
            echo ">>> Building Paxos binary..."
            make clean && make -j32
            ;;
        raft-single)
            echo ">>> Building Single-Raft binary..."
            make clean && make mako-raft-single -j32
            ;;
        raft-multi)
            echo ">>> Building Multi-Raft binary..."
            make clean && make mako-raft-multi -j32
            ;;
        *)
            echo "ERROR: Unknown backend '$backend'"
            continue
            ;;
    esac

    if [ $? -ne 0 ]; then
        echo "ERROR: Build failed for $backend, skipping."
        FAILED_BACKENDS="$FAILED_BACKENDS $backend"
        continue
    fi

    echo ""
    echo ">>> Running sweep for $backend..."
    bash scripts/run_scalability_sweep.sh --backend "$backend" --threads "$THREADS" --runs "$RUNS"

    if [ $? -eq 0 ]; then
        SUCCEEDED_BACKENDS="$SUCCEEDED_BACKENDS $backend"
    else
        FAILED_BACKENDS="$FAILED_BACKENDS $backend"
    fi
done

echo ""
echo "############################################################"
echo "  ALL SWEEPS COMPLETE"
echo "  Finished: $(date)"
echo "  Succeeded:$SUCCEEDED_BACKENDS"
if [ -n "$FAILED_BACKENDS" ]; then
    echo "  Failed:   $FAILED_BACKENDS"
fi
echo "############################################################"
echo ""
echo "Results are in results/benchmarks/{paxos,raft-single,raft-multi}/scalability_latest/"
