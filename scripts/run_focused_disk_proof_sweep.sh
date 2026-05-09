#!/bin/bash
set -euo pipefail

cd /home/users/mmakadia/mako

THREADS="${THREADS:-1 6 11}"
STAMP="${STAMP:-$(date +%Y%m%d_%H%M%S)}"
PROOF_OUT="results/benchmarks/disk_compare_replay"
mkdir -p "$PROOF_OUT"

export MPLCONFIGDIR="${MPLCONFIGDIR:-/home/users/mmakadia/.local/tmp/matplotlib}"
mkdir -p "$MPLCONFIGDIR"

echo "============================================================"
echo "  Focused disk proof sweep"
echo "  Started: $(date)"
echo "  Host:    $(hostname)"
echo "  Threads: $THREADS"
echo "============================================================"

echo
echo ">>> Building no-disk binaries"
cmake -S . -B build -DMAKO_USE_RAFT=ON -DSINGLE_RAFT_INSTANCE=ON -DDISABLE_DISK=ON
cmake --build build --target dbtest --parallel 32

cmake -S . -B build_multi -DMAKO_USE_RAFT=ON -DSINGLE_RAFT_INSTANCE=OFF -DDISABLE_DISK=ON
cmake --build build_multi --target dbtest --parallel 32

cmake -S . -B build_paxos -DMAKO_USE_RAFT=OFF -DDISABLE_DISK=ON
cmake --build build_paxos --target dbtest --parallel 32

echo
echo ">>> Building disk-enabled binaries"
cmake -S . -B build_disk -DMAKO_USE_RAFT=ON -DSINGLE_RAFT_INSTANCE=ON -DDISABLE_DISK=OFF
cmake --build build_disk --target dbtest --parallel 32

cmake -S . -B build_multi_disk -DMAKO_USE_RAFT=ON -DSINGLE_RAFT_INSTANCE=OFF -DDISABLE_DISK=OFF
cmake --build build_multi_disk --target dbtest --parallel 32

cmake -S . -B build_paxos_disk -DMAKO_USE_RAFT=OFF -DDISABLE_DISK=OFF
cmake --build build_paxos_disk --target dbtest --parallel 32

echo
echo ">>> Running no-disk proof points"
THREADS="$THREADS" INTER_RUN_SLEEP="${INTER_RUN_SLEEP:-30}" \
    bash scripts/run_non_persistence_sweep.sh
NODISK_DIR="$(readlink -f results/benchmarks/non-persistence-results/latest)"
echo "No-disk results: $NODISK_DIR"

echo
echo ">>> Running NVMe proof points"
THREADS="$THREADS" \
DISK_LABEL=nvme \
MAKO_PERSIST_BW_MBPS=3000 \
MAKO_PERSIST_LATENCY_US=100 \
    bash scripts/run_simulated_persistence_sweep.sh
NVME_DIR="$(readlink -f results/benchmarks/simulated-persistence-results/latest)"
echo "NVMe results: $NVME_DIR"

echo
echo ">>> Running Cloud-SSD proof points"
THREADS="$THREADS" \
DISK_LABEL=cloudssd \
MAKO_PERSIST_BW_MBPS=1000 \
MAKO_PERSIST_LATENCY_US=1000 \
    bash scripts/run_simulated_persistence_sweep.sh
CLOUDSSD_DIR="$(readlink -f results/benchmarks/simulated-persistence-results/latest)"
echo "Cloud-SSD results: $CLOUDSSD_DIR"

echo
echo ">>> Generating proof table"
python3 scripts/disk_proof_table.py \
    --nodisk "$NODISK_DIR" \
    --nvme "$NVME_DIR" \
    --cloudssd "$CLOUDSSD_DIR" \
    --out "$PROOF_OUT/disk_proof_table_${STAMP}.md"
cp "$PROOF_OUT/disk_proof_table_${STAMP}.md" "$PROOF_OUT/disk_proof_table.md"

cat > "$PROOF_OUT/disk_proof_latest_paths_${STAMP}.txt" <<EOF
nodisk=$NODISK_DIR
nvme=$NVME_DIR
cloudssd=$CLOUDSSD_DIR
table=$PROOF_OUT/disk_proof_table_${STAMP}.md
EOF
cp "$PROOF_OUT/disk_proof_latest_paths_${STAMP}.txt" "$PROOF_OUT/disk_proof_latest_paths.txt"

echo
echo "============================================================"
echo "  Focused disk proof sweep finished: $(date)"
echo "  No-disk:   $NODISK_DIR"
echo "  NVMe:      $NVME_DIR"
echo "  Cloud-SSD: $CLOUDSSD_DIR"
echo "  Table:     $PROOF_OUT/disk_proof_table_${STAMP}.md"
echo "============================================================"
