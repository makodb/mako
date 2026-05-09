#!/bin/bash
# Create a clean thesis-facing index over the benchmark results.
#
# This intentionally does not delete or move raw benchmark output. Benchmark
# scripts continue writing under results/benchmarks/, while this directory gives
# the thesis work one stable place to inspect the runs that matter.

set -euo pipefail

REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$REPO"

DEST="${DEST:-results/thesis_results}"
mkdir -p \
    "$DEST/01_no_disk_four_way" \
    "$DEST/02_replay_pool_sensitivity" \
    "$DEST/03_disk_persistence" \
    "$DEST/04_variance" \
    "$DEST/logs"

abs_path() {
    if [ -e "$1" ] || [ -L "$1" ]; then
        readlink -f "$1"
    fi
}

link_path() {
    local target="$1"
    local link="$2"

    if [ -e "$target" ] || [ -L "$target" ]; then
        rm -rf "$link"
        ln -s "$(abs_path "$target")" "$link"
    fi
}

latest_glob() {
    local pattern="$1"
    ls -td $pattern 2>/dev/null | head -1 || true
}

FOUR_WAY="$(abs_path results/benchmarks/overnight_four_way_latest || true)"
REPLAY="$(latest_glob 'results/benchmarks/replay_pool_sweep_*')"
FINAL="$(abs_path results/benchmarks/final_thesis_sweeps_latest || true)"
RESUME_LOG="$(latest_glob 'sweep_logs/final_thesis_resume_*.log')"
FAILED_WRAPPER_LOG="$(latest_glob 'sweep_logs/final_thesis_sweeps_*.log')"

if [ -n "${FOUR_WAY:-}" ]; then
    link_path "$FOUR_WAY" "$DEST/01_no_disk_four_way/raw_run"
    link_path "$FOUR_WAY/single_raft_no_pool/results.csv" "$DEST/01_no_disk_four_way/single_raft_no_pool.csv"
    link_path "$FOUR_WAY/single_raft_pool.csv" "$DEST/01_no_disk_four_way/single_raft_1to1_replay_pool.csv"
    link_path "$FOUR_WAY/multi_raft/results.csv" "$DEST/01_no_disk_four_way/multi_raft.csv"
    link_path "$FOUR_WAY/paxos/results.csv" "$DEST/01_no_disk_four_way/paxos.csv"
fi

if [ -n "${REPLAY:-}" ]; then
    link_path "$REPLAY" "$DEST/02_replay_pool_sensitivity/raw_run"
    link_path "$REPLAY/summary.csv" "$DEST/02_replay_pool_sensitivity/summary.csv"
fi

if [ -n "${FINAL:-}" ]; then
    link_path "$FINAL" "$DEST/current_resume_or_final_run"
    link_path "$FINAL/manifest.txt" "$DEST/manifest.txt"
    link_path "$FINAL/disk_proof/no_disk" "$DEST/03_disk_persistence/no_disk"
    link_path "$FINAL/disk_proof/nvme" "$DEST/03_disk_persistence/nvme"
    link_path "$FINAL/disk_proof/cloudssd" "$DEST/03_disk_persistence/cloudssd"
    link_path "$FINAL/disk_proof/disk_proof_table.md" "$DEST/03_disk_persistence/disk_proof_table.md"
    link_path "$FINAL/variance" "$DEST/04_variance/raw_runs"
fi

if [ -n "${RESUME_LOG:-}" ]; then
    link_path "$RESUME_LOG" "$DEST/logs/current_resume.log"
fi

if [ -n "${FAILED_WRAPPER_LOG:-}" ]; then
    link_path "$FAILED_WRAPPER_LOG" "$DEST/logs/failed_wrapper_before_resume.log"
fi

cat > "$DEST/README.md" <<EOF
# Thesis Results Index

Generated: $(date -Is)

This folder is the clean entry point for thesis results. It contains symlinks
to the raw benchmark outputs under \`results/benchmarks/\`; raw data has not
been deleted or moved.

## Main Results

- \`01_no_disk_four_way/\`: completed full t=1..11 no-disk comparison.
- \`02_replay_pool_sensitivity/\`: ReplayPool sensitivity at t=11. This may be
  incomplete while the resume sweep is still running.
- \`03_disk_persistence/\`: no-disk, NVMe, and Cloud-SSD proof sweeps. Links
  appear here as the resume sweep reaches those stages.
- \`04_variance/\`: headline reruns. Links appear after the resume sweep reaches
  the variance stage.
- \`logs/\`: detached sweep logs.

## Known Raw Runs

- Completed no-disk four-way: ${FOUR_WAY:-not found}
- Current final/resume run: ${FINAL:-not found}
- Current ReplayPool sweep: ${REPLAY:-not found}
- Current resume log: ${RESUME_LOG:-not found}
EOF

echo "Organized thesis results at: $DEST"
