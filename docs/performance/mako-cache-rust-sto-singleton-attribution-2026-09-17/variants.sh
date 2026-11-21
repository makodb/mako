#!/usr/bin/env bash
set -uo pipefail
P=/var/tmp/mako-residual-gap.7FPbwg
W=/var/tmp/mako-engine-perf-20260912.iz63eX
OUT=/var/tmp/mako-attr-20260917.3; mkdir -p $OUT
export LD_LIBRARY_PATH="$W:/home/users/shuai/.linuxbrew/opt/llvm@22/lib"
export LD_PRELOAD="$W/libjemalloc.so.2"
for r in 1 2 3; do
 for v in baseline:$P/probe-sto rcu:$P/rcu holder:$P/holder; do
  tag=${v%%:*}; bin=${v##*:}
  taskset -c 33-63 "$bin" engine --workers 1 --iterations 262144 --warmup 65536 --perf 1 2>/dev/null \
   | python3 -c "
import json,sys
r=json.loads(sys.stdin.read())
print(f\"r$r {('$tag'):9s} tx/s {r['transactions_per_second']:>12,.0f} cyc/tx {r['cycles_per_commit']:>8.1f} ins/tx {r['instructions_per_commit']:>7.0f}\")"
 done
done
