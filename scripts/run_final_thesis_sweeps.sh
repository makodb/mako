#!/bin/bash
# Final overnight thesis sweep driver.
#
# This script clean-builds every backend used by the thesis experiments, then
# runs the result set needed for the paper-style thesis:
#   1. Four-way no-disk scalability:
#      Single-Raft no pool, Single-Raft 1:1 ReplayPool, Multi-Raft, Paxos.
#   2. ReplayPool sensitivity at a fixed worker count.
#   3. Full disk-proof sweep:
#      no-disk, simulated NVMe, simulated Cloud-SSD at t=1..11 by default.
#   4. Optional headline t=11 variance reruns.
#
# The underlying CSVs include the thesis-critical evidence:
#   - throughput_ops_sec from the leader's committed benchmark throughput
#   - worker_mean_cpu_pct, worker_peak_cpu_pct, role_worker_mean/peak
#   - replay_batch_p1/p2 for follower replay sanity, not headline throughput
#   - FakeDisk byte/write counters for persistence proof
#
# Detached usage:
#   mkdir -p sweep_logs
#   nohup bash scripts/run_final_thesis_sweeps.sh \
#     > sweep_logs/final_thesis_sweeps_$(date +%Y%m%d_%H%M%S).log 2>&1 &
#
# Useful overrides:
#   THREADS_FULL="1 2 3 4 5 6 7 8 9 10 11"
#   THREADS_DISK="1 2 3 4 5 6 7 8 9 10 11"
#   RUN_BUILD=0
#   RUN_FULL=0
#   RUN_VARIANCE=0
#   BUILD_PARALLEL=32

set -euo pipefail

REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$REPO"

STAMP="$(date +%Y%m%d_%H%M%S)"
OUT="${OUT:-results/benchmarks/final_thesis_sweeps_${STAMP}}"
LOG_ROOT="${LOG_ROOT:-sweep_logs}"
BUILD_PARALLEL="${BUILD_PARALLEL:-32}"

THREADS_FULL="${THREADS_FULL:-1 2 3 4 5 6 7 8 9 10 11}"
THREADS_DISK="${THREADS_DISK:-1 2 3 4 5 6 7 8 9 10 11}"
REPLAY_SENSITIVITY_THREADS="${REPLAY_SENSITIVITY_THREADS:-11}"
REPLAY_SENSITIVITY_SIZES="${REPLAY_SENSITIVITY_SIZES:-0 1 2 4 8 11}"

RUN_FULL="${RUN_FULL:-1}"
RUN_REPLAY="${RUN_REPLAY:-1}"
RUN_DISK="${RUN_DISK:-1}"
RUN_VARIANCE="${RUN_VARIANCE:-1}"
RUN_BUILD="${RUN_BUILD:-1}"
ALLOW_EXISTING_DBTEST="${ALLOW_EXISTING_DBTEST:-0}"

NVME_BW_MBPS="${NVME_BW_MBPS:-3000}"
NVME_LATENCY_US="${NVME_LATENCY_US:-100}"
CLOUDSSD_BW_MBPS="${CLOUDSSD_BW_MBPS:-1000}"
CLOUDSSD_LATENCY_US="${CLOUDSSD_LATENCY_US:-1000}"

mkdir -p "$OUT/logs" "$LOG_ROOT"
OUT="$(readlink -f "$OUT")"
ln -sfnT "$OUT" results/benchmarks/final_thesis_sweeps_latest

export MPLCONFIGDIR="${MPLCONFIGDIR:-/tmp/mako-mpl-${USER}}"
mkdir -p "$MPLCONFIGDIR"

MANIFEST="$OUT/manifest.txt"

log() {
    printf '[%s] %s\n' "$(date -Is)" "$*"
}

append_manifest() {
    printf '%s\n' "$*" >> "$MANIFEST"
}

run_logged() {
    local name="$1"
    shift
    local logfile="$OUT/logs/${name}.log"

    log "START $name"
    {
        printf 'step=%s\n' "$name"
        printf 'started=%s\n' "$(date -Is)"
        "$@"
        printf 'finished=%s\n' "$(date -Is)"
    } 2>&1 | tee "$logfile"
    log "DONE  $name"
}

check_no_existing_dbtest() {
    if [ "$ALLOW_EXISTING_DBTEST" = "1" ]; then
        log "ALLOW_EXISTING_DBTEST=1, skipping dbtest preflight."
        return
    fi

    local existing
    existing="$(pgrep -u "$USER" -af '[d]btest' || true)"
    if [ -n "$existing" ]; then
        cat >&2 <<EOF
ERROR: dbtest processes are already running for this user.
Set ALLOW_EXISTING_DBTEST=1 only if you intentionally want to overlap runs.

$existing
EOF
        exit 1
    fi
}

configure_and_build() {
    local dir="$1"
    shift

    log "Clean configuring $dir"
    rm -rf "$dir"
    cmake -S . -B "$dir" "$@" -DENABLE_CCACHE=OFF
    cmake --build "$dir" --target dbtest --parallel "$BUILD_PARALLEL"
}

build_all() {
    configure_and_build build \
        -DMAKO_USE_RAFT=ON \
        -DSINGLE_RAFT_INSTANCE=ON \
        -DDISABLE_DISK=ON

    configure_and_build build_multi \
        -DMAKO_USE_RAFT=ON \
        -DSINGLE_RAFT_INSTANCE=OFF \
        -DDISABLE_DISK=ON

    configure_and_build build_paxos \
        -DMAKO_USE_RAFT=OFF \
        -DDISABLE_DISK=ON

    configure_and_build build_disk \
        -DMAKO_USE_RAFT=ON \
        -DSINGLE_RAFT_INSTANCE=ON \
        -DDISABLE_DISK=OFF

    configure_and_build build_multi_disk \
        -DMAKO_USE_RAFT=ON \
        -DSINGLE_RAFT_INSTANCE=OFF \
        -DDISABLE_DISK=OFF

    configure_and_build build_paxos_disk \
        -DMAKO_USE_RAFT=OFF \
        -DDISABLE_DISK=OFF
}

run_four_way() {
    THREADS="$THREADS_FULL" bash scripts/overnight_four_way.sh
}

run_replay_sensitivity() {
    bash scripts/sweep_replay_pool.sh \
        "$REPLAY_SENSITIVITY_THREADS" \
        "$REPLAY_SENSITIVITY_SIZES"
}

run_nodisk_proof() {
    THREADS="$THREADS_DISK" INTER_RUN_SLEEP=30 \
        bash scripts/run_non_persistence_sweep.sh
}

run_nvme_proof() {
    THREADS="$THREADS_DISK" \
    DISK_LABEL=nvme \
    MAKO_PERSIST_BW_MBPS="$NVME_BW_MBPS" \
    MAKO_PERSIST_LATENCY_US="$NVME_LATENCY_US" \
    INTER_RUN_SLEEP=30 \
        bash scripts/run_simulated_persistence_sweep.sh
}

run_cloudssd_proof() {
    THREADS="$THREADS_DISK" \
    DISK_LABEL=cloudssd \
    MAKO_PERSIST_BW_MBPS="$CLOUDSSD_BW_MBPS" \
    MAKO_PERSIST_LATENCY_US="$CLOUDSSD_LATENCY_US" \
    INTER_RUN_SLEEP=30 \
        bash scripts/run_simulated_persistence_sweep.sh
}

copy_latest_dir() {
    local src="$1"
    local dst="$2"

    if [ -e "$src" ]; then
        rm -rf "$dst"
        mkdir -p "$dst"
        cp -a "$src/." "$dst/"
        append_manifest "$dst=$(readlink -f "$dst")"
    fi
}

run_single_t11_variance() {
    mkdir -p "$OUT/variance/raw/single_raft"
    BENCH_ROOT_OVERRIDE="$(readlink -f "$OUT/variance/raw/single_raft")" \
    BUILD_DIR=build \
    MAKO_REPLAY_THREADS=11 \
    INTER_RUN_SLEEP=30 \
        bash scripts/run_scalability_sweep.sh \
            --backend raft-single \
            --threads "11" \
            --runs 3 \
            --batch-size 400
}

run_multi_t11_variance() {
    mkdir -p "$OUT/variance/raw/multi_raft"
    BENCH_ROOT_OVERRIDE="$(readlink -f "$OUT/variance/raw/multi_raft")" \
    BUILD_DIR=build_multi \
    MAKO_REPLAY_THREADS=11 \
    INTER_RUN_SLEEP=30 \
        bash scripts/run_scalability_sweep.sh \
            --backend raft-multi \
            --threads "11" \
            --runs 3 \
            --batch-size 400
}

run_paxos_t11_variance() {
    mkdir -p "$OUT/variance/raw/paxos"
    BENCH_ROOT_OVERRIDE="$(readlink -f "$OUT/variance/raw/paxos")" \
    BUILD_DIR=build_paxos \
    MAKO_REPLAY_THREADS=11 \
    INTER_RUN_SLEEP=30 \
        bash scripts/run_scalability_sweep.sh \
            --backend paxos \
            --threads "11" \
            --runs 3 \
            --batch-size 400
}

run_cloudssd_t11_confirmation() {
    THREADS="11" \
    DISK_LABEL=cloudssd_t11_confirmation \
    MAKO_PERSIST_BW_MBPS="$CLOUDSSD_BW_MBPS" \
    MAKO_PERSIST_LATENCY_US="$CLOUDSSD_LATENCY_US" \
    INTER_RUN_SLEEP=30 \
        bash scripts/run_simulated_persistence_sweep.sh
}

generate_disk_table() {
    local nodisk="$1"
    local nvme="$2"
    local cloudssd="$3"

    mkdir -p "$OUT/disk_proof" results/benchmarks/disk_compare_replay
    python3 scripts/disk_proof_table.py \
        --nodisk "$nodisk" \
        --nvme "$nvme" \
        --cloudssd "$cloudssd" \
        --out "$OUT/disk_proof/disk_proof_table.md"
    cp "$OUT/disk_proof/disk_proof_table.md" \
        results/benchmarks/disk_compare_replay/disk_proof_table.md
}

{
    printf 'kind=final-thesis-sweeps\n'
    printf 'started=%s\n' "$(date -Is)"
    printf 'host=%s\n' "$(hostname)"
    printf 'repo=%s\n' "$REPO"
    printf 'git_commit=%s\n' "$(git rev-parse HEAD 2>/dev/null || echo unknown)"
    printf 'threads_full=%s\n' "$THREADS_FULL"
    printf 'threads_disk=%s\n' "$THREADS_DISK"
    printf 'replay_sensitivity_threads=%s\n' "$REPLAY_SENSITIVITY_THREADS"
    printf 'replay_sensitivity_sizes=%s\n' "$REPLAY_SENSITIVITY_SIZES"
    printf 'run_full=%s\n' "$RUN_FULL"
    printf 'run_replay=%s\n' "$RUN_REPLAY"
    printf 'run_disk=%s\n' "$RUN_DISK"
    printf 'run_variance=%s\n' "$RUN_VARIANCE"
    printf 'run_build=%s\n' "$RUN_BUILD"
    printf 'build_parallel=%s\n' "$BUILD_PARALLEL"
} > "$MANIFEST"

log "Final thesis sweeps output: $OUT"
check_no_existing_dbtest
if [ "$RUN_BUILD" = "1" ]; then
    run_logged build_all build_all
else
    log "Skipping clean build because RUN_BUILD=0"
fi

if [ "$RUN_FULL" = "1" ]; then
    run_logged four_way_nodisk run_four_way
    FOUR_WAY_LATEST="$(readlink -f results/benchmarks/overnight_four_way_latest)"
    copy_latest_dir "$FOUR_WAY_LATEST" "$OUT/four_way_nodisk"
fi

if [ "$RUN_REPLAY" = "1" ]; then
    run_logged replay_pool_sensitivity run_replay_sensitivity
    REPLAY_LATEST="$(ls -td results/benchmarks/replay_pool_sweep_* 2>/dev/null | head -1 || true)"
    if [ -n "$REPLAY_LATEST" ]; then
        copy_latest_dir "$REPLAY_LATEST" "$OUT/replay_pool_sensitivity"
    fi
fi

if [ "$RUN_DISK" = "1" ]; then
    run_logged disk_proof_nodisk run_nodisk_proof
    NODISK_LATEST="$(readlink -f results/benchmarks/non-persistence-results/latest)"
    copy_latest_dir "$NODISK_LATEST" "$OUT/disk_proof/no_disk"

    run_logged disk_proof_nvme run_nvme_proof
    NVME_LATEST="$(readlink -f results/benchmarks/simulated-persistence-results/latest)"
    copy_latest_dir "$NVME_LATEST" "$OUT/disk_proof/nvme"

    run_logged disk_proof_cloudssd run_cloudssd_proof
    CLOUDSSD_LATEST="$(readlink -f results/benchmarks/simulated-persistence-results/latest)"
    copy_latest_dir "$CLOUDSSD_LATEST" "$OUT/disk_proof/cloudssd"

    run_logged disk_proof_table generate_disk_table \
        "$OUT/disk_proof/no_disk" \
        "$OUT/disk_proof/nvme" \
        "$OUT/disk_proof/cloudssd"
fi

if [ "$RUN_VARIANCE" = "1" ]; then
    mkdir -p "$OUT/variance"
    run_logged variance_single_raft_t11 run_single_t11_variance
    run_logged variance_multi_raft_t11 run_multi_t11_variance
    run_logged variance_paxos_t11 run_paxos_t11_variance
    run_logged cloudssd_t11_confirmation run_cloudssd_t11_confirmation
    CLOUDSSD_T11_LATEST="$(readlink -f results/benchmarks/simulated-persistence-results/latest)"
    copy_latest_dir "$CLOUDSSD_T11_LATEST" "$OUT/variance/cloudssd_t11_confirmation"
fi

append_manifest "finished=$(date -Is)"

log "All requested thesis sweeps finished."
log "Manifest: $MANIFEST"
log "Latest symlink: results/benchmarks/final_thesis_sweeps_latest"
