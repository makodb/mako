#!/usr/bin/env bash
# ABBA paired trial of two raft_bench builds at one point.
#
#   scripts/raft_perf/paired_trial.sh OUT_DIR PAIRS ARM_A ARM_B
#
# Pair i runs A then B when i is odd and B then A when i is even, so slow
# drift in the host cancels instead of biasing one arm. Each arm is a build
# directory under the repo root holding a raft_bench. Point parameters come
# from the environment, as in arms_roundrobin.sh: PAYLOAD (4096), RATE (240,
# 0 = unlimited), MAXOUT (4096), DUR (8 s), PARTS (1), SNAPSHOT_BYTES (0 =
# none; snapshots then also need MAKO_RAFT_SNAPSHOTS=1 and an interval in the
# environment, which the launcher passes through), STALL_AT / STALL_FOR (0 =
# no stall).
#
# Prints, per metric, the median of the per-pair ratio B/A - 1 and a two-sided
# sign test over the pairs -- the statistics the earlier paired trials in
# docs/migration/raft/paired-trial-*.csv reported. Records land in OUT_DIR.
set -euo pipefail
OUT="$1"; PAIRS="$2"; A="$3"; B="$4"
REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
PAYLOAD="${PAYLOAD:-4096}"; RATE="${RATE:-240}"; MAXOUT="${MAXOUT:-4096}"
DUR="${DUR:-8}"; PARTS="${PARTS:-1}"
SNAPSHOT_BYTES="${SNAPSHOT_BYTES:-0}"; STALL_AT="${STALL_AT:-0}"; STALL_FOR="${STALL_FOR:-0}"
MIN_PAIRS="${MIN_PAIRS:-$(( PAIRS * 23 / 25 ))}"
mkdir -p "$OUT"
run() {
  local arm="$1" f="$2"
  (cd "$REPO_ROOT" && examples/raft_bench.sh --build-dir "$arm" --out "$f" \
     --partitions "$PARTS" --payload-bytes "$PAYLOAD" --rate "$RATE" \
     --max-outstanding "$MAXOUT" --duration-sec "$DUR" \
     --snapshot-bytes "$SNAPSHOT_BYTES" --stall-follower-at-sec "$STALL_AT" \
     --stall-for-sec "$STALL_FOR" >"$f.log" 2>&1)
}
for i in $(seq 1 "$PAIRS"); do
  if [ $((i % 2)) -eq 1 ]; then order="$A $B"; else order="$B $A"; fi
  for arm in $order; do
    run "$arm" "$OUT/p$i.$arm.json" || echo "pair $i $arm FAILED"
  done
  echo "pair $i done"
done
python3 - "$OUT" "$PAIRS" "$A" "$B" "$MIN_PAIRS" <<'PY'
import json, math, os, statistics, sys
out, pairs, a, b, min_pairs = sys.argv[1], int(sys.argv[2]), sys.argv[3], sys.argv[4], int(sys.argv[5])
def load(i, arm):
    f = os.path.join(out, f"p{i}.{arm}.json")
    try:
        return json.load(open(f))
    except Exception:
        return None
def sign_p(pos, neg):
    n = pos + neg
    if n == 0:
        return 1.0
    k = min(pos, neg)
    tail = sum(math.comb(n, j) for j in range(0, k + 1)) / 2 ** n
    return min(1.0, 2 * tail)
rows = []
for i in range(1, pairs + 1):
    ra, rb = load(i, a), load(i, b)
    if ra and rb:
        rows.append((ra, rb))
print(f"{len(rows)} complete pairs of {pairs}: B={b} against A={a}")
def value(r, key):
    # "<stalled>_x": the stalled follower's merged side-record field.
    if key.startswith("stalled_"):
        v = r.get("stalled_follower")
        return r.get(f"{v}_{key[len('stalled_'):]}") if v else None
    return r.get(key)
METRICS = (("applied_per_sec", "higher"), ("latency_p50_us", "lower"),
           ("latency_p99_us", "lower"), ("latency_p999_us", "lower"),
           ("latency_max_us", "lower"), ("max_apply_gap_us", "lower"),
           ("snapshot_create_us_p50", "lower"), ("install_rpcs_sent", "lower"),
           ("install_bytes_sent", "lower"), ("rss_peak_kb", "lower"),
           ("stalled_snapshot_install_us_p50", "lower"), ("stalled_catchup_ms", "lower"),
           ("stalled_rss_peak_kb", "lower"))
for key, better in METRICS:
    ratios = [value(rb, key) / value(ra, key) - 1 for ra, rb in rows
              if value(ra, key) and value(rb, key) and value(ra, key) > 0]
    if not ratios:
        continue
    pos = sum(1 for r in ratios if r > 0)
    neg = sum(1 for r in ratios if r < 0)
    med = statistics.median(ratios)
    print(f"  {key:16s} median B/A-1 = {med:+.2%}  (B>A in {pos}, B<A in {neg}; "
          f"sign test p = {sign_p(pos, neg):.3f}; {better} is better)")
PY
status=$?
# A pair that failed is dropped from the statistics above; too many dropped
# pairs mean the comparison is not the one asked for.
done_pairs=$(for i in $(seq 1 "$PAIRS"); do [ -s "$OUT/p$i.$A.json" ] && [ -s "$OUT/p$i.$B.json" ] && echo x; done | wc -l)
if [ "$done_pairs" -lt "$MIN_PAIRS" ]; then
    echo "paired_trial: FAILED — only $done_pairs of $PAIRS pairs completed (need $MIN_PAIRS)" >&2
    exit 1
fi
exit $status
