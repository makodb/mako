#!/bin/bash
# Freeze the exact data files used by the thesis graph scripts.

set -euo pipefail

REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$REPO"

SRC_ROOT="results/thesis_results"
DEST_ROOT="doc/thesis/figures/graphs/data_snapshot"
MANIFEST="$DEST_ROOT/manifest.csv"
README="$DEST_ROOT/README.md"

mkdir -p "$DEST_ROOT"

printf "graph_id,source_path,snapshot_path,sha256\n" > "$MANIFEST"

copy_for_graph() {
    local graph_id="$1"
    local source_path="$2"
    local snapshot_rel="$3"
    local snapshot_path="$DEST_ROOT/$snapshot_rel"

    if [ ! -f "$source_path" ]; then
        echo "missing source file: $source_path" >&2
        exit 1
    fi

    mkdir -p "$(dirname "$snapshot_path")"
    cp -L "$source_path" "$snapshot_path"
    local hash
    hash="$(sha256sum "$snapshot_path" | awk '{print $1}')"
    printf "%s,%s,%s,%s\n" "$graph_id" "$source_path" "$snapshot_path" "$hash" >> "$MANIFEST"
}

# fig06 and fig07: no-disk scalability and worker CPU.
copy_for_graph fig06_fig07 "$SRC_ROOT/01_no_disk_four_way/single_raft_no_pool.csv" "01_no_disk_four_way/single_raft_no_pool.csv"
copy_for_graph fig06_fig07 "$SRC_ROOT/01_no_disk_four_way/single_raft_1to1_replay_pool.csv" "01_no_disk_four_way/single_raft_1to1_replay_pool.csv"
copy_for_graph fig06_fig07 "$SRC_ROOT/01_no_disk_four_way/multi_raft.csv" "01_no_disk_four_way/multi_raft.csv"
copy_for_graph fig06_fig07 "$SRC_ROOT/01_no_disk_four_way/paxos.csv" "01_no_disk_four_way/paxos.csv"

# fig08: current fixed-t=11 ReplayPool sensitivity.
copy_for_graph fig08 "$SRC_ROOT/02_replay_pool_sensitivity/summary.csv" "02_replay_pool_sensitivity/summary.csv"
for n in 0 1 2 4 8 11; do
    copy_for_graph fig08 "$SRC_ROOT/02_replay_pool_sensitivity/raw_run/n${n}/results.csv" "02_replay_pool_sensitivity/n${n}/results.csv"
done

# fig09, fig10, and fig11: disk persistence and FakeDisk proof.
for disk in no_disk nvme cloudssd; do
    for backend in single_raft multi_raft paxos; do
        copy_for_graph fig09_fig10_fig11 "$SRC_ROOT/03_disk_persistence/${disk}/${backend}/results.csv" "03_disk_persistence/${disk}/${backend}/results.csv"
    done
done
copy_for_graph fig09_fig10_fig11 "$SRC_ROOT/03_disk_persistence/disk_proof_table.md" "03_disk_persistence/disk_proof_table.md"

# fig12: headline variance.
copy_for_graph fig12 "$SRC_ROOT/04_variance/raw_runs/raw/single_raft/scalability_20260506_005116/results.csv" "04_variance/single_raft_t11/results.csv"
copy_for_graph fig12 "$SRC_ROOT/04_variance/raw_runs/raw/multi_raft/scalability_20260506_005527/results.csv" "04_variance/multi_raft_t11/results.csv"
copy_for_graph fig12 "$SRC_ROOT/04_variance/raw_runs/raw/paxos/scalability_20260506_005942/results.csv" "04_variance/paxos_t11/results.csv"
copy_for_graph fig12 "doc/thesis/figures/graphs/headline_variance_t11.csv" "04_variance/headline_variance_t11.csv"

if [ -f "$SRC_ROOT/02_replay_pool_matrix/summary.csv" ]; then
    copy_for_graph fig08_matrix "$SRC_ROOT/02_replay_pool_matrix/summary.csv" "02_replay_pool_matrix/summary.csv"
fi

{
    printf '%s\n\n' "# Thesis Graph Data Snapshot"
    printf '%s\n\n' 'This folder freezes the exact result files used by the current thesis graphs in `doc/thesis/figures/graphs/`.'
    printf '%s\n' "- Git branch: \`$(git branch --show-current)\`"
    printf '%s\n' "- Git commit: \`$(git rev-parse HEAD)\`"
    printf '%s\n' "- Source root: \`$SRC_ROOT\`"
    printf '%s\n\n' '- Manifest: `manifest.csv`'
    printf '%s\n\n' 'Use these files when quoting numbers in the evaluation chapter, so the prose matches the generated figures even if later benchmark runs update `results/thesis_results/`.'
    if [ -f "$DEST_ROOT/02_replay_pool_matrix/summary.csv" ]; then
        printf '%s\n' 'ReplayPool matrix note: the full `t=1..11` matrix sweep is included in this snapshot.'
    else
        printf '%s\n' 'ReplayPool matrix note: the full `t=1..11` matrix sweep was not available when this snapshot was made.'
    fi
} > "$README"

echo "Frozen graph data at: $DEST_ROOT"
echo "Manifest: $MANIFEST"
