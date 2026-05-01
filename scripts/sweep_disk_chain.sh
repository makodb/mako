#!/bin/bash
# Chain: cloud-SSD sweep -> plot+insights -> NVMe sweep -> plot+insights.
#
# Designed to be launched detached so it survives the launching session
# being closed (PC shutdown, ssh disconnect, etc.):
#
#   nohup setsid bash scripts/sweep_disk_chain.sh \
#     > results/benchmarks/_chain_$(date +%Y%m%d_%H%M%S).log 2>&1 &
#
# Total wall time: ~10 hours (5 hr cloud-SSD + 5 hr NVMe).

set -e

REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$REPO"

run_sweep_and_analyze() {
    local sweep_script="$1"
    local class_label="$2"

    echo
    echo "================================================================="
    echo "  CHAIN STEP: $class_label sweep starting at $(date)"
    echo "================================================================="
    bash "scripts/$sweep_script"
    echo
    echo "================================================================="
    echo "  CHAIN STEP: $class_label sweep done at $(date)"
    echo "================================================================="

    local link="results/benchmarks/disk_${class_label}_latest"
    if [ ! -L "$link" ]; then
        echo "ERROR: $link symlink missing — sweep may have failed."
        return 1
    fi
    local rel
    rel=$(readlink "$link")
    local full_dir="results/benchmarks/$rel"

    echo
    echo "================================================================="
    echo "  CHAIN STEP: plotting $class_label  ($full_dir)"
    echo "================================================================="
    mkdir -p "$full_dir/plots"
    python3 scripts/plot_overnight_four_way.py \
        --no-pool "$full_dir/single_raft_no_pool_disk/results.csv" \
        --pool    "$full_dir/single_raft_pool_disk/results.csv" \
        --multi   "$full_dir/multi_raft_disk/results.csv" \
        --paxos   "$full_dir/paxos_disk/results.csv" \
        --outdir  "$full_dir/plots"

    echo
    echo "================================================================="
    echo "  CHAIN STEP: insights for $class_label"
    echo "================================================================="
    python3 scripts/disk_class_insights.py "$full_dir" \
        | tee "$full_dir/insights.md"
}

echo "================================================================="
echo "  DISK-CLASS SWEEP CHAIN"
echo "  Started: $(date)"
echo "  Order:   cloud-SSD (5 hr)  ->  NVMe (5 hr)"
echo "================================================================="

run_sweep_and_analyze sweep_disk_cloudssd.sh cloudssd
run_sweep_and_analyze sweep_disk_nvme.sh     nvme

echo
echo "================================================================="
echo "  CHAIN DONE at $(date)"
echo
echo "  Result dirs:"
echo "    - results/benchmarks/disk_cloudssd_latest -> $(readlink results/benchmarks/disk_cloudssd_latest)"
echo "    - results/benchmarks/disk_nvme_latest     -> $(readlink results/benchmarks/disk_nvme_latest)"
echo "  Insights:  *_latest/insights.md"
echo "  Plots:     *_latest/plots/dashboard.png + per-metric PNGs"
echo "================================================================="
