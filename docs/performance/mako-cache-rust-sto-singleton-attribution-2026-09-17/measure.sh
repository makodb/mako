#!/usr/bin/env bash
# Fresh matched controls on zoo-002: frozen pair (cpp-thin/stage3-thin) and probe pair,
# engine mode at 1 worker, with the bench's own PMU counters.
set -uo pipefail
W=/var/tmp/mako-engine-perf-20260912.iz63eX
F=/var/tmp/mako-w1-fixes.ShjYqz
P=/var/tmp/mako-residual-gap.7FPbwg
OUT=${1:?out dir}
mkdir -p "$OUT"
export LD_LIBRARY_PATH="$W:/home/users/shuai/.linuxbrew/opt/llvm@22/lib"
export LD_PRELOAD="$W/libjemalloc.so.2"
run() { # tag binary
  local tag=$1 bin=$2
  taskset -c 33-63 "$bin" engine --workers 1 --iterations 262144 --warmup 65536 --perf 1 \
    2> "$OUT/$tag.stderr" | tee -a "$OUT/$tag.jsonl" | \
    python3 -c "
import json,sys
r=json.loads(sys.stdin.read())
print(f\"  {r['engine']:24s} tx/s {r['transactions_per_second']:>12,.0f}  cyc/tx {r['cycles_per_commit']:>8.1f}  ins/tx {r['instructions_per_commit']:>7.0f}  pmu {r['pmu_min_running_ratio']:.4f}\")"
}
for r in 1 2 3; do
  echo "--- round $r ---"
  run "frozen-cpp-r$r" "$F/cpp-thin"
  run "frozen-sto-r$r" "$F/stage3-thin"
done
echo "--- probe pair ---"
run "probe-cpp-r1" "$P/probe-c-abi"
run "probe-sto-r1" "$P/probe-sto"
echo "--- perf stat extended (frozen pair) ---"
for arm in cpp "$F/cpp-thin" sto "$F/stage3-thin"; do :; done
perf stat -e cycles:u,instructions:u,branches:u,branch-misses:u,stalled-cycles-frontend:u,stalled-cycles-backend:u,L1-dcache-loads:u,L1-dcache-load-misses:u,LLC-loads:u,LLC-load-misses:u,dTLB-load-misses:u \
  -o "$OUT/stat-cpp.txt" -- taskset -c 33-63 "$F/cpp-thin" engine --workers 1 --iterations 262144 --warmup 65536 --perf 0 >/dev/null 2>&1
perf stat -e cycles:u,instructions:u,branches:u,branch-misses:u,stalled-cycles-frontend:u,stalled-cycles-backend:u,L1-dcache-loads:u,L1-dcache-load-misses:u,LLC-loads:u,LLC-load-misses:u,dTLB-load-misses:u \
  -o "$OUT/stat-sto.txt" -- taskset -c 33-63 "$F/stage3-thin" engine --workers 1 --iterations 262144 --warmup 65536 --perf 0 >/dev/null 2>&1
echo "=== stat cpp ==="; cat "$OUT/stat-cpp.txt"
echo "=== stat sto ==="; cat "$OUT/stat-sto.txt"
