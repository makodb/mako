#!/usr/bin/env bash
# ABBA paired trial of two raft_bench builds at one point.
#
#   scripts/raft_perf/paired_trial.sh OUT_DIR PAIRS ARM_A ARM_B
#
# Pair i runs A then B when i is odd and B then A when i is even, so slow
# drift in the host cancels instead of biasing one arm. Each arm is a build
# directory under the repo root holding a raft_bench. Point parameters come
# from the environment, as in arms_roundrobin.sh: PAYLOAD (4096), RATE (240,
# 0 = unlimited), MAXOUT (4096), DUR (8 s), PARTS (1).
#
# Prints, per metric, the median of the per-pair ratio B/A - 1 and a two-sided
# sign test over the pairs -- the statistics the earlier paired trials in
# docs/migration/raft/paired-trial-*.csv reported. Records land in OUT_DIR.
set -euo pipefail
OUT="$1"; PAIRS="$2"; A="$3"; B="$4"
REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
PAYLOAD="${PAYLOAD:-4096}"; RATE="${RATE:-240}"; MAXOUT="${MAXOUT:-4096}"
DUR="${DUR:-8}"; PARTS="${PARTS:-1}"
mkdir -p "$OUT"
run() {
  local arm="$1" f="$2"
  (cd "$REPO_ROOT" && examples/raft_bench.sh --build-dir "$arm" --out "$f" \
     --partitions "$PARTS" --payload-bytes "$PAYLOAD" --rate "$RATE" \
     --max-outstanding "$MAXOUT" --duration-sec "$DUR" >"$f.log" 2>&1)
}
for i in $(seq 1 "$PAIRS"); do
  if [ $((i % 2)) -eq 1 ]; then order="$A $B"; else order="$B $A"; fi
  for arm in $order; do
    run "$arm" "$OUT/p$i.$arm.json" || echo "pair $i $arm FAILED"
  done
  echo "pair $i done"
done
python3 - "$OUT" "$PAIRS" "$A" "$B" <<'PY'
import json, math, os, statistics, sys
out, pairs, a, b = sys.argv[1], int(sys.argv[2]), sys.argv[3], sys.argv[4]
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
for key, better in (("applied_per_sec", "higher"), ("latency_p50_us", "lower"),
                    ("latency_p99_us", "lower")):
    ratios = [rb[key] / ra[key] - 1 for ra, rb in rows if ra.get(key) and rb.get(key)]
    if not ratios:
        continue
    pos = sum(1 for r in ratios if r > 0)
    neg = sum(1 for r in ratios if r < 0)
    med = statistics.median(ratios)
    print(f"  {key:16s} median B/A-1 = {med:+.2%}  (B>A in {pos}, B<A in {neg}; "
          f"sign test p = {sign_p(pos, neg):.3f}; {better} is better)")
PY
