#!/bin/bash
# 4-backend throttled-disk sweep.
#
# Same four conditions as scripts/overnight_four_way.sh (no-disk), but built
# with DISABLE_DISK=OFF so Mako's RocksDB layer is active, persistence files
# redirected to /dev/shm (tmpfs), and a per-write throttle injected so the
# tmpfs-backed disk *behaves like* a real disk with the configured bandwidth
# and latency limits.
#
# Conditions, all at t=1..11:
#   1) Single-Raft, NO replay pool   (MAKO_REPLAY_THREADS=0, build_disk/)
#   2) Single-Raft, replay pool 1:1   (MAKO_REPLAY_THREADS=$t, build_disk/)
#   3) Multi-Raft  with replay pool   (MAKO_REPLAY_THREADS=$t, build_multi_disk/)
#   4) Paxos       with replay pool   (MAKO_REPLAY_THREADS=$t, build_paxos_disk/)
#
# Throttle is configured by the caller via two env vars:
#   MAKO_PERSIST_BW_MBPS    bandwidth ceiling, MB/s
#   MAKO_PERSIST_LATENCY_US per-write minimum latency, microseconds
#   DISK_LABEL              short tag for the result dir (e.g. "nvme", "cloudssd")
#
# Output dir:  results/benchmarks/disk_${DISK_LABEL}_4backends_<STAMP>/
# Symlink:     results/benchmarks/disk_${DISK_LABEL}_latest -> ...
#
# Total runtime: ~5 hours per disk class.
#
# Usage (typically via the wrapper scripts sweep_disk_nvme.sh /
# sweep_disk_cloudssd.sh):
#   DISK_LABEL=nvme MAKO_PERSIST_BW_MBPS=3000 MAKO_PERSIST_LATENCY_US=100 \
#       nohup bash scripts/sweep_disk_throttled.sh > disk_nvme.log 2>&1 &

set -e

REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$REPO"

# Required configuration ------------------------------------------------------
: "${DISK_LABEL:?DISK_LABEL must be set (e.g. nvme, cloudssd)}"
: "${MAKO_PERSIST_BW_MBPS:?MAKO_PERSIST_BW_MBPS must be set (MB/s)}"
: "${MAKO_PERSIST_LATENCY_US:?MAKO_PERSIST_LATENCY_US must be set (us)}"
# Activate the FakeDisk simulator. Without this the binary takes the
# real-RocksDB code path and the throttle never fires.
export MAKO_PERSIST_FAKE_DISK=1
export MAKO_PERSIST_BW_MBPS MAKO_PERSIST_LATENCY_US

THREADS="${THREADS:-1 2 3 4 5 6 7 8 9 10 11}"
PERSIST_ROOT="${MAKO_PERSIST_ROOT:-/dev/shm/mako_persist}"
export MAKO_PERSIST_ROOT="$PERSIST_ROOT"

STAMP=$(date +%Y%m%d_%H%M%S)
OUT="results/benchmarks/disk_${DISK_LABEL}_4backends_$STAMP"
mkdir -p "$OUT" "$PERSIST_ROOT"

echo "================================================================="
echo "  Throttled-disk 4-backend sweep — class: $DISK_LABEL"
echo "  Started:        $(date)"
echo "  Output:         $OUT"
echo "  Threads:        $THREADS"
echo "  Persist root:   $PERSIST_ROOT  (tmpfs, RAM-backed)"
echo "  Throttle:       BW=${MAKO_PERSIST_BW_MBPS} MB/s, "\
"LAT=${MAKO_PERSIST_LATENCY_US} us"
echo "================================================================="

# Pre-flight: confirm persistence binaries exist.
for d in build_disk build_multi_disk build_paxos_disk; do
    if [ ! -x "$REPO/$d/dbtest" ]; then
        echo "ERROR: $d/dbtest missing — rebuild before running."
        exit 1
    fi
done
echo "Persistence binaries verified:"
ls -l build_disk/dbtest build_multi_disk/dbtest build_paxos_disk/dbtest

# Pre-flight: confirm /dev/shm has space.
SHM_AVAIL=$(df -BG --output=avail "$PERSIST_ROOT" | tail -1 | tr -d ' G')
if [ "$SHM_AVAIL" -lt 16 ]; then
    echo "ERROR: persistence root has only ${SHM_AVAIL}G free; need at least 16G."
    exit 1
fi

clean_persist_root() {
    rm -rf "$PERSIST_ROOT"/* 2>/dev/null || true
}

CSV_HEADER="threads,run,throughput_ops_sec,per_core_throughput,avg_cpu_pct,peak_cpu_pct,avg_latency_ms,avg_persist_latency_ms,agg_abort_rate,replay_batch_p1,replay_batch_p2,active_threads,worker_mean_cpu_pct,worker_peak_cpu_pct,role_worker_mean,role_worker_peak,role_replay_mean,role_replay_peak,role_apply_peak,role_other_mean,exit_code,replay_threads"

# -----------------------------------------------------------------------------
# Helper: run one backend (label, build_dir, backend_name, sweep_dir_base,
# replay_threads_expr) where replay_threads_expr is either "0" (no pool) or
# "$t" (1:1 pool).
# -----------------------------------------------------------------------------
run_one_backend() {
    local label="$1" build="$2" backend="$3" sweep_dir_base="$4" rt_expr="$5"
    echo
    echo "================================================================="
    echo "  ${label}  ($(date))"
    echo "================================================================="
    local backend_out="$OUT/${label}"
    mkdir -p "$backend_out/logs"
    local backend_csv="$backend_out/results.csv"
    echo "$CSV_HEADER" > "$backend_csv"

    for t in $THREADS; do
        local rt
        if [ "$rt_expr" = "0" ]; then rt=0; else rt=$t; fi
        echo "--- ${label}, t=$t (MAKO_REPLAY_THREADS=$rt, "\
"BW=${MAKO_PERSIST_BW_MBPS} MB/s, LAT=${MAKO_PERSIST_LATENCY_US} us) ---"
        clean_persist_root
        BUILD_DIR="$build" \
        MAKO_REPLAY_THREADS="$rt" \
        INTER_RUN_SLEEP=30 \
          bash scripts/run_scalability_sweep.sh \
            --backend "$backend" --threads "$t" --runs 1 --batch-size 400
        local src
        src=$(readlink "results/benchmarks/${sweep_dir_base}/scalability_latest")
        cp "$src/logs/"* "$backend_out/logs/" 2>/dev/null || true
        tail -n +2 "$src/results.csv" | while IFS= read -r row; do
            printf "%s,%s\n" "$row" "$rt" >> "$backend_csv"
        done
    done
    echo "${label} done at $(date).  CSV: $backend_csv"
}

# 1) Single-Raft, NO replay pool (the broken baseline, with disk amplifier)
run_one_backend "single_raft_no_pool_disk" build_disk       raft-single raft-single 0

# 2) Single-Raft + 1:1 replay pool
run_one_backend "single_raft_pool_disk"    build_disk       raft-single raft-single t

# 3) Multi-Raft
run_one_backend "multi_raft_disk"          build_multi_disk raft-multi  raft-multi  t

# 4) Paxos
run_one_backend "paxos_disk"               build_paxos_disk paxos       paxos       t

# Final cleanup of persist root.
clean_persist_root

ln -sfT "$(basename "$OUT")" "results/benchmarks/disk_${DISK_LABEL}_latest"

echo
echo "================================================================="
echo "  THROTTLED-DISK SWEEP DONE — class: $DISK_LABEL  ($(date))"
echo "  Output dir: $OUT"
echo "================================================================="
