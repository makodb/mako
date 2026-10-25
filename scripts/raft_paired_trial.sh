#!/usr/bin/env bash
# Paired throughput trial between two Mako trees, for the Raft conversion.
#
# Runs ./ci/ci.sh shard1ReplicationRaft alternately from tree A and tree B --
# order flipped every pair (ABBA) so slow drift on the host cancels -- and
# records agg_persist_throughput per run. A run that fails its own checks is
# recorded as a failure and excluded; the verdict is on completed pairs only.
#
#   bash scripts/raft_paired_trial.sh <treeA> <treeB> <pairs> <out.csv>
#
# Each tree must already have a production build at <tree>/build/dbtest. Runs
# are sequential (the suites share ports). Analyse with
#   python3 scripts/raft_paired_trial.py <out.csv>
set -u
A=$1; B=$2; PAIRS=$3; OUT=$4
export PATH="/usr/bin:$PATH"
export LD_LIBRARY_PATH="$HOME/.local/mako-deps/usr/lib/x86_64-linux-gnu:${LD_LIBRARY_PATH:-}"
export LIBRARY_PATH="$HOME/.local/mako-deps/usr/lib/x86_64-linux-gnu:${LIBRARY_PATH:-}"
echo "pair,order,tree,throughput,exit" > "$OUT"
one() {  # tree label pair order
  local tree=$1 label=$2 pair=$3 order=$4 log tp rc
  log=$(mktemp "/tmp/raft_paired_${label}_XXXX.log")
  ( cd "$tree" && ./ci/ci.sh shard1ReplicationRaft ) > "$log" 2>&1; rc=$?
  tp=$(grep -oE 'agg_persist_throughput: [0-9.]+' "$log" | tail -1 | grep -oE '[0-9.]+$')
  echo "$pair,$order,$label,${tp:-NA},$rc" >> "$OUT"
  echo "pair $pair $order $label: ${tp:-NA} ops/s (exit $rc)"
  rm -f "$log"
}
for ((p = 1; p <= PAIRS; p++)); do
  if (( p % 2 == 1 )); then
    one "$A" A "$p" 1; one "$B" B "$p" 2
  else
    one "$B" B "$p" 1; one "$A" A "$p" 2
  fi
done
