#!/usr/bin/env bash
# Rotated trial of several raft_bench builds ("arms") at one point.
#
#   scripts/raft_perf/rotation_trial.sh OUT_DIR ROUNDS arm...
#
# Round i runs every arm once, starting from arm (i mod K) and wrapping
# around, so over K rounds each arm runs in every position once. Drift in the
# host is then spread evenly over every arm, as ABBA does for two. Several
# comparisons can be drawn from one set of runs without re-running shared
# arms. Records land in OUT_DIR as r<i>.<arm>.json. Compare any two arms with
# scripts/raft_perf/paired_stats.py OUT_DIR ROUNDS A B.
#
# Point parameters come from the environment, as in paired_trial.sh: PAYLOAD
# (4096), RATE (240, 0 = unlimited), MAXOUT (4096), DUR (8 s), PARTS (1),
# SNAPSHOT_BYTES (0 = none; snapshots then also need MAKO_RAFT_SNAPSHOTS=1
# and MAKO_RAFT_SNAPSHOT_INTERVAL in the environment, which the launcher
# passes through), STALL_AT / STALL_FOR (0 = no stall), GROUP (single|multi,
# raft_bench.sh --group-mode).
set -euo pipefail
OUT="$1"; ROUNDS="$2"; shift 2
ARMS=("$@")
K=${#ARMS[@]}
REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
PAYLOAD="${PAYLOAD:-4096}"; RATE="${RATE:-240}"; MAXOUT="${MAXOUT:-4096}"
DUR="${DUR:-8}"; PARTS="${PARTS:-1}"; GROUP="${GROUP:-single}"
SNAPSHOT_BYTES="${SNAPSHOT_BYTES:-0}"; STALL_AT="${STALL_AT:-0}"; STALL_FOR="${STALL_FOR:-0}"
mkdir -p "$OUT"
run() {
  local arm="$1" f="$2"
  (cd "$REPO_ROOT" && examples/raft_bench.sh --build-dir "$arm" --out "$f" \
     --partitions "$PARTS" --group-mode "$GROUP" --payload-bytes "$PAYLOAD" --rate "$RATE" \
     --max-outstanding "$MAXOUT" --duration-sec "$DUR" \
     --snapshot-bytes "$SNAPSHOT_BYTES" --stall-follower-at-sec "$STALL_AT" \
     --stall-for-sec "$STALL_FOR" >"$f.log" 2>&1)
}
for i in $(seq 1 "$ROUNDS"); do
  for j in $(seq 0 $((K - 1))); do
    arm="${ARMS[$(( (i + j) % K ))]}"
    f="$OUT/r$i.$arm.json"
    [ -s "$f" ] && continue   # resumable: a finished run is kept
    run "$arm" "$f" || { echo "round $i $arm FAILED (see $f.log)"; rm -f "$f"; }
  done
  echo "round $i done"
done
