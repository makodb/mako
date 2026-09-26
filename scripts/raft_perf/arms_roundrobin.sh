#!/usr/bin/env bash
# Round-robin a raft_bench point over several preserved binaries ("arms"), so
# host drift hits every arm equally. Each arm is a build directory under the
# repo root holding a raft_bench (a symlink to a preserved copy is fine: see
# docs/performance/raft-latency-regression.md for why arms are preserved).
#
#   scripts/raft_perf/arms_roundrobin.sh OUT_DIR TRIALS DURATION_SEC arm...
#
# Writes OUT_DIR/<arm>.t<N>.json and prints one line per run:
#   <arm> tN applied/s X p50 Y p90 Z p99 W     (latencies in microseconds)
set -euo pipefail
OUT="$1"; TRIALS="$2"; DUR="$3"; shift 3
ARMS=("$@")
REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
PAYLOAD="${PAYLOAD:-4096}"; RATE="${RATE:-240}"; PARTS="${PARTS:-1}"
mkdir -p "$OUT"
summ() {
  python3 - "$1" <<'PY'
import json,sys
d=json.load(open(sys.argv[1]))
g=lambda k: d.get(k, float('nan'))
print(f"applied/s {g('applied_per_sec'):7.1f}  p50 {g('latency_p50_us'):7.0f}  p90 {g('latency_p90_us'):7.0f}  p99 {g('latency_p99_us'):7.0f}  gaps {g('gaps')} dup {g('duplicates')} ooo {g('out_of_order')}")
PY
}
for t in $(seq 1 "$TRIALS"); do
  for arm in "${ARMS[@]}"; do
    f="$OUT/$arm.t$t.json"
    (cd "$REPO_ROOT" && examples/raft_bench.sh --build-dir "$arm" --out "$f" \
       --partitions "$PARTS" --payload-bytes "$PAYLOAD" --rate "$RATE" \
       --duration-sec "$DUR" >"$OUT/$arm.t$t.log" 2>&1) || { echo "$arm t$t FAILED"; continue; }
    printf '%-18s t%-2s %s\n' "$arm" "$t" "$(summ "$f")"
  done
done
