#!/usr/bin/env bash
# Post-fix-plan measurements on the freshly built HEAD binaries:
#   1) per-transaction copies/allocations/clock reads for the sto arm
#   2) --items 1/2/4 curve for both arms
set -uo pipefail
BIN=/var/tmp/probe-mine/bin
W=/var/tmp/mako-engine-perf-20260912.iz63eX
export LD_LIBRARY_PATH="$W:/home/users/shuai/.linuxbrew/opt/llvm@22/lib"
JM="$W/libjemalloc.so.2"
out=/var/tmp/mako-head-20260918; mkdir -p "$out"

echo "=== HEAD sto: PMU + shims (engine, 1 worker) ==="
for r in 1 2; do
  for shim in none memcount alloccount clockcount; do
    case $shim in
      none) pre="$JM";;
      memcount) pre="$JM /var/tmp/memcount.so";;
      alloccount) pre="/var/tmp/alloccount.so $JM";;
      clockcount) pre="$JM /var/tmp/clockcount.so";;
    esac
    LD_PRELOAD="$pre" taskset -c 33-63 "$BIN/engine-compare-items-sto" engine \
      --workers 1 --iterations 262144 --warmup 65536 --perf 1 \
      2>"$out/head-$shim-r$r.err" | python3 -c "
import json,sys
r=json.loads(sys.stdin.read())
print(f\"r$r $shim      tx/s {r['transactions_per_second']:>11,.0f}  cyc/tx {r['cycles_per_commit']:>8.1f}  ins/tx {r['instructions_per_commit']:>7.0f}\")"
    grep -hE "MEMCOUNT|ALLOCCOUNT" "$out/head-$shim-r$r.err" | sed "s/^/   r$r /"
    grep -h "CLOCKCOUNT" "$out/head-$shim-r$r.err" | sort -t= -k3 -rn | head -n 1 | sed "s/^/   r$r worker /"
  done
done

echo "=== --items curve (HEAD, both arms) ==="
for r in 1 2 3; do
  for arm in c-abi sto; do
    for items in 1 2 4; do
      bin="$BIN/engine-compare-items-$arm"; [ -x "$bin" ] || continue
      taskset -c 33-63 "$bin" engine --workers 1 --iterations 262144 --warmup 65536 \
        --perf 1 --items "$items" 2>"$out/items-$r-$arm-$items.err" \
        >"$out/items-$r-$arm-$items.json"
      python3 - "$out/items-$r-$arm-$items.json" "$r" "$arm" "$items" <<'PY'
import json,sys
try:
    r=json.load(open(sys.argv[1]))
except Exception:
    print(f"r{sys.argv[2]} {sys.argv[3]:5s} items={sys.argv[4]} FAILED"); raise SystemExit
print(f"r{sys.argv[2]} {sys.argv[3]:5s} items={sys.argv[4]} tx/s {r['transactions_per_second']:>11,.0f} "
      f"cyc/tx {r['cycles_per_commit']:>8.1f} ins/tx {r['instructions_per_commit']:>7.0f} "
      f"ins/item {r['instructions_per_commit']/int(sys.argv[4]):>7.1f} verified={r['verified_keys']}")
PY
    done
  done
done
chown -R shuai:shuai "$out" /var/tmp/probe-mine 2>/dev/null
echo done
