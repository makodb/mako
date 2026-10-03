#!/usr/bin/env bash
# g7.sh PHASE ARM_A ARM_B [KILLS]
#
# Point G7 of docs/verus/modification-plan.md §6: election and failover time.
# KILLS (default 20) leader kills per arm, the arms interleaved kill by kill
# in alternating order (A B, B A, ...) so host drift spreads over both rather
# than looking like a difference between them. Each run is
# examples/raft_bench.sh --kill-leader-at-sec with the failover-timing record
# (raft_bench --failover-out); records land in $RESULTS/PHASE/G7/<i>.<arm>.json.
#
# MAKO_RAFT_PREFERRED_GRACE_US is 6 s, not the launcher's 30 s: under 30 s the
# kill lands inside the surviving replicas' preferred-leader grace window and
# G7 would measure their 5-10 s grace election timeouts. With 6 s the windows
# end at about 6 s and 8 s after the first replica starts, and the kill, 6 s
# after the leader starts offering (about 10.5 s), lands after both.
#
# Then runs scripts/verus/election_times.py ARM_A vs ARM_B (pass --aa through
# G7_AA=1 for the Phase 0 A/A run, which sets the bounds instead).
set -uo pipefail
P=${1:?usage: g7.sh PHASE ARM_A ARM_B [KILLS]}; A=${2:?}; B=${3:?}; K=${4:-20}
: "${RESULTS:?source ~/mako-verus-env.sh first}"
: "${PY:?source ~/mako-verus-env.sh first}"
REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
cd "$REPO_ROOT"
OUT=$RESULTS/$P/G7
mkdir -p "$OUT"
uptime > "$OUT.uptime"
for i in $(seq 1 "$K"); do
  if [ $((i % 2)) -eq 1 ]; then order="$A $B"; else order="$B $A"; fi
  for arm in $order; do
    f="$OUT/$i.$arm.json"
    [ -s "$f" ] && continue   # resumable
    MAKO_RAFT_PREFERRED_GRACE_US=6000000 timeout 600 examples/raft_bench.sh \
      --build-dir "$arm" --out "$f" --payload-bytes 4096 --rate 240 \
      --max-outstanding 4096 --duration-sec 15 --kill-leader-at-sec 6 \
      > "$OUT/$i.$arm.log" 2>&1 || echo "kill $i $arm: launcher exit $?"
  done
  echo "kill $i done"
done
"$PY" scripts/verus/election_times.py "$OUT" "$A" "$B" ${G7_AA:+--aa}
