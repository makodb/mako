#!/usr/bin/env bash
# gate_point.sh PHASE POINT PARENT CHILD [BASE]
#
# Runs one Tier 2 point of docs/verus/modification-plan.md §6 (rotating all
# given arms with scripts/raft_perf/rotation_trial.sh), then applies the pass
# rule PARENT vs CHILD and, if BASE is given, BASE vs CHILD (the cumulative
# gate). Rounds and bounds come from the point's row in
# docs/verus/gate-params.md: | G1 | <rounds> | <metric=bound,...> | <note> |
#
# Raw results land in $RESULTS/PHASE/POINT (never in git). Exits 1 if any
# comparison fails, 2 on a usage or configuration error.
#
# G2 is gated on a traced metric (decided after the Phase 3 checkpoint,
# the Phase 3 report §3.1, since removed): the rotation runs with the Phase 0
# trace kit on and scripts/verus/two_follower_rounds.py turns each run into
# the time of its rounds that sent to both followers. G2_REPORT_ROUNDS (25)
# untraced rounds follow in $OUT/untraced for the reported, ungated lines:
# throughput and how many runs end with a follower behind.
set -euo pipefail
if [ $# -lt 4 ]; then
  echo "usage: $0 PHASE POINT PARENT CHILD [BASE]" >&2
  exit 2
fi
P=$1 G=$2 A=$3 C=$4 BASE=${5:-}
: "${RESULTS:?source ~/mako-verus-env.sh first}"
REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
cd "$REPO_ROOT"
case $G in
  G1) export PAYLOAD=4096    RATE=240 MAXOUT=4096 PARTS=1 GROUP=single;;
  G2) export PAYLOAD=4096    RATE=0   MAXOUT=4096 PARTS=1 GROUP=single;;
  G3) export PAYLOAD=286208  RATE=190 MAXOUT=256  PARTS=6 GROUP=multi;;
  G4) export PAYLOAD=286208  RATE=0   MAXOUT=256  PARTS=6 GROUP=multi;;
  G5) export PAYLOAD=1048576 RATE=55  MAXOUT=64   PARTS=1 GROUP=single;;
  G6) export PAYLOAD=1048576 RATE=0   MAXOUT=64   PARTS=1 GROUP=single;;
  *) echo "unknown point $G" >&2; exit 2;;
esac
export DUR=10
row=$(grep -E "^\| $G \|" docs/verus/gate-params.md) || { echo "no row for $G in gate-params.md" >&2; exit 2; }
N=$(echo "$row" | awk -F'|' '{gsub(/ /,"",$3); print $3}')
B=$(echo "$row" | awk -F'|' '{gsub(/ /,"",$4); print $4}')
mkdir -p "$RESULTS/$P"; OUT=$RESULTS/$P/$G
mkdir -p "$OUT"; uptime > "$OUT.uptime"
echo "gate_point: $P $G rounds=$N bounds=$B arms: $A $C ${BASE}"
if [ "$G" = G2 ]; then
  TRACE=1 scripts/raft_perf/rotation_trial.sh "$OUT" "$N" "$A" "$C" ${BASE:+"$BASE"}
else
  scripts/raft_perf/rotation_trial.sh "$OUT" "$N" "$A" "$C" ${BASE:+"$BASE"}
fi
# rotation_trial.sh deletes a round's JSON when raft_bench.sh failed but keeps
# its .log, so a log integrity violation would otherwise look like one
# missing pair.
if grep -l 'log integrity violation' "$OUT"/*.log 2>/dev/null; then
  echo "GATE FAIL $G: log integrity"; exit 1
fi
rc=0
STATS=scripts/raft_perf/paired_stats.py
if [ "$G" = G2 ]; then
  python3 scripts/verus/two_follower_rounds.py summarize "$OUT" "$N" "$A" "$C" ${BASE:+"$BASE"} || rc=1
  STATS="scripts/verus/two_follower_rounds.py stats"
fi
python3 $STATS "$OUT" "$N" "$A" "$C" --json > "$OUT/$A-vs-$C.json" || rc=1
python3 scripts/verus/perf_gate.py "$OUT/$A-vs-$C.json" --bounds "$B" || rc=1
if [ -n "$BASE" ]; then
  python3 $STATS "$OUT" "$N" "$BASE" "$C" --json > "$OUT/$BASE-vs-$C.json" || rc=1
  python3 scripts/verus/perf_gate.py "$OUT/$BASE-vs-$C.json" --bounds "$B" || { echo "GATE FAIL $G cumulative"; rc=1; }
fi
if [ "$G" = G2 ]; then
  # Reported beside the gate, never gated.
  U=$OUT/untraced R=${G2_REPORT_ROUNDS:-25}
  scripts/raft_perf/rotation_trial.sh "$U" "$R" "$A" "$C" ${BASE:+"$BASE"}
  echo "G2 reported, not gated (untraced, $R rounds):"
  python3 scripts/raft_perf/paired_stats.py "$U" "$R" "${BASE:-$A}" "$C" | grep -E "rounds|applied_per_sec" || true
  python3 scripts/verus/follower_behind.py "$U" "$R" "${BASE:-$A}" "$C" ${BASE:+"$A"}
fi
exit $rc
