#!/bin/bash
# Smoke test for the foreground fake-disk path (MAKO_PERSIST_FAKE_DISK=1).
#
# Runs t=1,3,5 for multi-raft and paxos under two conditions:
#   A) FAKE_DISK off  → should match the no-disk baseline (sanity check)
#   B) FAKE_DISK on, BW=1000 MB/s, LAT=1000 us (Cloud-SSD class) → expect
#      a measurable throughput drop because the sleep is on the commit path.
#
# Output: results/benchmarks/smoke_fake_disk_<STAMP>/
#         summary.md   side-by-side table
#         <case>/results.csv

set -e
REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$REPO"

STAMP=$(date +%Y%m%d_%H%M%S)
OUT="results/benchmarks/smoke_fake_disk_$STAMP"
mkdir -p "$OUT"

THREADS="1 3 5"
PERSIST_ROOT="${MAKO_PERSIST_ROOT:-/dev/shm/mako_persist}"
mkdir -p "$PERSIST_ROOT"
export MAKO_PERSIST_ROOT="$PERSIST_ROOT"

CSV_HEADER="threads,run,throughput_ops_sec,per_core_throughput,avg_cpu_pct,peak_cpu_pct,avg_latency_ms,avg_persist_latency_ms,agg_abort_rate,replay_batch_p1,replay_batch_p2,active_threads,worker_mean_cpu_pct,worker_peak_cpu_pct,role_worker_mean,role_worker_peak,role_replay_mean,role_replay_peak,role_apply_peak,role_other_mean"

clean_persist_root() {
    rm -rf "$PERSIST_ROOT"/* 2>/dev/null || true
}

# run_case  <label>  <build>  <backend>  <fake_disk>  <bw>  <lat>
run_case() {
    local label="$1" build="$2" backend="$3" fake="$4" bw="$5" lat="$6"
    echo
    echo "========================================================="
    echo "  $label   ($(date))"
    echo "    build=$build backend=$backend"
    echo "    FAKE_DISK=$fake  BW=$bw MB/s  LAT=$lat us"
    echo "========================================================="
    local case_out="$OUT/$label"
    mkdir -p "$case_out/logs"
    echo "$CSV_HEADER" > "$case_out/results.csv"

    for t in $THREADS; do
        echo "--- $label, t=$t ---"
        clean_persist_root
        BUILD_DIR="$build" \
        MAKO_REPLAY_THREADS="$t" \
        MAKO_PERSIST_FAKE_DISK="$fake" \
        MAKO_PERSIST_BW_MBPS="$bw" \
        MAKO_PERSIST_LATENCY_US="$lat" \
        INTER_RUN_SLEEP=15 \
          bash scripts/run_scalability_sweep.sh \
            --backend "$backend" --threads "$t" --runs 1 --batch-size 400
        local src
        src=$(readlink "results/benchmarks/${backend}/scalability_latest")
        cp "$src/logs/"* "$case_out/logs/" 2>/dev/null || true
        tail -n +2 "$src/results.csv" >> "$case_out/results.csv"
    done
    echo "$label done at $(date)"
}

echo "========================================================="
echo "  FAKE-DISK SMOKE TEST — Started $(date)"
echo "  Output: $OUT"
echo "  Threads: $THREADS"
echo "========================================================="

run_case multi_raft_off    build_multi_disk raft-multi 0 0    0
run_case multi_raft_cloudssd build_multi_disk raft-multi 1 1000 1000
run_case paxos_off         build_paxos_disk paxos      0 0    0
run_case paxos_cloudssd    build_paxos_disk paxos      1 1000 1000

clean_persist_root

ln -sfT "$(basename "$OUT")" "results/benchmarks/smoke_fake_disk_latest"

echo
echo "========================================================="
echo "  SMOKE TEST DONE — $(date)"
echo "  Output: $OUT"
echo "========================================================="
