#!/bin/bash
# Overnight three-way SIMULATED-PERSISTENCE sweep.
#
# Same three backends as the no-persistence overnight, but built with
# DISABLE_DISK=OFF and FakeDisk enabled so the Mako persistence path pays
# a shared queued disk cost configured by:
#   MAKO_PERSIST_BW_MBPS
#   MAKO_PERSIST_LATENCY_US
#   DISK_LABEL
#
#   Conditions (all at t=1..11, MAKO_REPLAY_THREADS=$t):
#     1) Single-Raft + replay pool (build_disk/)
#     2) Multi-Raft  (build_multi_disk/)
#     3) Paxos       (build_paxos_disk/)
#
# Output layout:
#   results/benchmarks/simulated-persistence-results/<DISK_LABEL>_<STAMP>/
#     single_raft/results.csv
#     multi_raft/results.csv
#     paxos/results.csv
#     plots/
#
# Total runtime: ~75 minutes.
#
# Usage:
#   nohup bash scripts/run_simulated_persistence_sweep.sh > simulated_persistence.log 2>&1 &

set -e

REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$REPO"

THREADS="${THREADS:-1 2 3 4 5 6 7 8 9 10 11}"
DISK_LABEL="${DISK_LABEL:-cloudssd}"
BW="${MAKO_PERSIST_BW_MBPS:-1000}"
LAT="${MAKO_PERSIST_LATENCY_US:-1000}"
STAMP=$(date +%Y%m%d_%H%M%S)
TOP="results/benchmarks/simulated-persistence-results"
OUT="$TOP/${DISK_LABEL}_$STAMP"
PERSIST_ROOT="${MAKO_PERSIST_ROOT:-/dev/shm/mako_persist}"
mkdir -p "$OUT" "$PERSIST_ROOT"

echo "================================================================="
echo "  Overnight three-way simulated-persistence sweep"
echo "  Started:        $(date)"
echo "  Output:         $OUT"
echo "  Threads:        $THREADS"
echo "  Disk label:     $DISK_LABEL"
echo "  Bandwidth:      ${BW} MB/s"
echo "  Latency:        ${LAT} us"
echo "  Persist root:   $PERSIST_ROOT"
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

cat > "$OUT/metadata.txt" <<EOF
kind=simulated-persistence
started=$(date -Is)
threads=$THREADS
disk_label=$DISK_LABEL
bandwidth_mbps=$BW
latency_us=$LAT
persist_root=$PERSIST_ROOT
single_build=build_disk
multi_build=build_multi_disk
paxos_build=build_paxos_disk
EOF

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
        echo "--- ${label}, t=$t (MAKO_REPLAY_THREADS=$t, BW=${BW} MB/s, LAT=${LAT} us) ---"
        clean_persist_root
        BUILD_DIR="$build" \
        MAKO_REPLAY_THREADS="$t" \
        MAKO_PERSIST_ROOT="$PERSIST_ROOT" \
        MAKO_PERSIST_FAKE_DISK=1 \
        MAKO_PERSIST_BW_MBPS="$BW" \
        MAKO_PERSIST_LATENCY_US="$LAT" \
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

# 1) Single-Raft + pool, with persistence simulation
run_one_backend "single_raft" build_disk raft-single raft-single

# 2) Multi-Raft, with persistence simulation
run_one_backend "multi_raft" build_multi_disk raft-multi raft-multi

# 3) Paxos, with persistence simulation
run_one_backend "paxos" build_paxos_disk paxos paxos

mkdir -p "$OUT/plots"
python3 scripts/plot_overnight_three_way.py \
    --single "$OUT/single_raft/results.csv" \
    --multi  "$OUT/multi_raft/results.csv" \
    --paxos  "$OUT/paxos/results.csv" \
    --outdir "$OUT/plots"

# Final cleanup of persist root so we don't leave 64GB of RocksDB sitting around.
clean_persist_root

ln -sfn "$(basename "$OUT")" "$TOP/latest"

echo
echo "================================================================="
echo "  SIMULATED-PERSISTENCE SWEEP DONE  ($(date))"
echo "  Output dir: $OUT"
echo "================================================================="
