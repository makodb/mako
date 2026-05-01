#!/bin/bash
# Overnight three-way TIER-1 PERSISTENCE sweep covering Step 3 of the plan.
#
# Same three "interesting" backends as the no-disk overnight, but built with
# DISABLE_DISK=OFF so the Mako RocksDB layer is active. MAKO_PERSIST_ROOT
# redirects the persistence files to /dev/shm (tmpfs) — modeling an
# "infinitely fast disk" so we measure the persistence *code path* cost
# without paying for the lab SSD's hardware speed.
#
#   Conditions (all at t=1..11, MAKO_REPLAY_THREADS=$t):
#     1) Single-Raft + replay pool (build_disk/)
#     2) Multi-Raft  (build_multi_disk/)
#     3) Paxos       (build_paxos_disk/)
#
# Note: per the post-smoke-test investigation, only Mako's RocksDB layer is
# actually exercised in the Mako benchmark path. The Raft consensus log
# (LogStorage) is never wired up under RaftWorker::SetupBase. So this sweep
# measures Mako database snapshot persistence cost specifically.
#
# Total runtime: ~75 minutes.
#
# Usage:
#   nohup bash scripts/overnight_three_way_with_disk.sh > overnight_disk.log 2>&1 &

set -e

REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$REPO"

THREADS="${THREADS:-1 2 3 4 5 6 7 8 9 10 11}"
STAMP=$(date +%Y%m%d_%H%M%S)
OUT="results/benchmarks/overnight_three_way_with_disk_$STAMP"
PERSIST_ROOT="${MAKO_PERSIST_ROOT:-/dev/shm/mako_persist}"
mkdir -p "$OUT" "$PERSIST_ROOT"

echo "================================================================="
echo "  Overnight three-way TIER-1 PERSISTENCE sweep (tmpfs)"
echo "  Started:        $(date)"
echo "  Output:         $OUT"
echo "  Threads:        $THREADS"
echo "  Persist root:   $PERSIST_ROOT  (tmpfs / RAM-backed)"
echo "================================================================="

# Pre-flight: confirm persistence-enabled binaries exist.
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

# Helper to wipe persistence root between runs so the sweep is reproducible.
clean_persist_root() {
    rm -rf "$PERSIST_ROOT"/* 2>/dev/null || true
}

run_one_backend() {
    local label="$1" build="$2" backend="$3" sweep_dir_base="$4"
    echo
    echo "================================================================="
    echo "  ${label}  ($(date))"
    echo "================================================================="
    local backend_out="$OUT/${label}"
    mkdir -p "$backend_out/logs"
    local backend_csv="$backend_out/results.csv"
    echo "threads,run,throughput_ops_sec,per_core_throughput,avg_cpu_pct,peak_cpu_pct,avg_latency_ms,avg_persist_latency_ms,agg_abort_rate,replay_batch_p1,replay_batch_p2,active_threads,worker_mean_cpu_pct,worker_peak_cpu_pct,role_worker_mean,role_worker_peak,role_replay_mean,role_replay_peak,role_apply_peak,role_other_mean,exit_code,replay_threads" > "$backend_csv"

    for t in $THREADS; do
        echo "--- ${label}, t=$t (MAKO_REPLAY_THREADS=$t, MAKO_PERSIST_ROOT=$PERSIST_ROOT) ---"
        clean_persist_root
        BUILD_DIR="$build" \
        MAKO_REPLAY_THREADS="$t" \
        MAKO_PERSIST_ROOT="$PERSIST_ROOT" \
        INTER_RUN_SLEEP=30 \
          bash scripts/run_scalability_sweep.sh \
            --backend "$backend" --threads "$t" --runs 1 --batch-size 400
        local src
        src=$(readlink "results/benchmarks/${sweep_dir_base}/scalability_latest")
        cp "$src/logs/"* "$backend_out/logs/" 2>/dev/null || true
        tail -n +2 "$src/results.csv" | while IFS= read -r row; do
            printf "%s,%s\n" "$row" "$t" >> "$backend_csv"
        done
    done
    echo "${label} done at $(date).  CSV: $backend_csv"
}

# 1) Single-Raft + pool, with persistence
run_one_backend "single_raft_pool_disk" build_disk raft-single raft-single

# 2) Multi-Raft, with persistence
run_one_backend "multi_raft_disk" build_multi_disk raft-multi raft-multi

# 3) Paxos, with persistence
run_one_backend "paxos_disk" build_paxos_disk paxos paxos

# Final cleanup of persist root so we don't leave 64GB of RocksDB sitting around.
clean_persist_root

ln -sfT "$(basename "$OUT")" "results/benchmarks/overnight_three_way_with_disk_latest"

echo
echo "================================================================="
echo "  TIER-1 PERSISTENCE SWEEP DONE  ($(date))"
echo "  Output dir: $OUT"
echo "================================================================="
