#!/usr/bin/env bash
# --items matrix: fixed per-transaction cost vs per-item cost, both lanes.
set -uo pipefail
W=/var/tmp/mako-engine-perf-20260912.iz63eX
export LD_LIBRARY_PATH="$W:/home/users/shuai/.linuxbrew/opt/llvm@22/lib"
export LD_PRELOAD="$W/libjemalloc.so.2"
out=/var/tmp/mako-items-20260917
mkdir -p "$out"
for r in 1 2 3; do
  for arm in c-abi sto; do
    for items in 1 2 4; do
      bin="/var/tmp/probe-mine/bin/engine-compare-items-$arm"; [ -x "$bin" ] || bin="/var/tmp/probe-mine/bin/probe-items-$arm"
      taskset -c 33-63 "$bin" engine \
        --workers 1 --iterations 262144 --warmup 65536 --perf 1 --items "$items" \
        2>"$out/stderr-$r-$arm-$items.txt" >"$out/out-$r-$arm-$items.json"
      python3 - "$out/out-$r-$arm-$items.json" "$r" "$arm" "$items" <<'PY'
import json,sys
r=json.load(open(sys.argv[1]))
print(f"r{sys.argv[2]} {sys.argv[3]:5s} items={sys.argv[4]} tx/s {r['transactions_per_second']:>11,.0f} "
      f"cyc/tx {r['cycles_per_commit']:>8.1f} ins/tx {r['instructions_per_commit']:>7.0f} "
      f"verified={r['verified_keys']} commits={r['commits']}")
PY
    done
  done
done
