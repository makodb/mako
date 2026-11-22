#!/usr/bin/env bash
# Instruction profiles of the HEAD binaries (no PROFILE_WINDOW markers: the
# busiest tid is the measured worker; warmup dilutes both lanes identically).
set -uo pipefail
BIN=/var/tmp/probe-mine/bin
W=/var/tmp/mako-engine-perf-20260912.iz63eX
export LD_LIBRARY_PATH="$W:/home/users/shuai/.linuxbrew/opt/llvm@22/lib"
export LD_PRELOAD="$W/libjemalloc.so.2"
out=/var/tmp/mako-head-prof-20260918; mkdir -p "$out"
for arm in sto c-abi; do
  perf record --clockid mono -e instructions:u -F 3999 --call-graph fp -o "$out/$arm.data" -- \
    taskset -c 33-63 "$BIN/engine-compare-items-$arm" engine --workers 1 \
    --iterations 16777216 --warmup 1048576 --perf 1 >"$out/$arm.json" 2>"$out/$arm.err"
  tid=$(perf report --stdio -i "$out/$arm.data" --no-children --sort tid --percent-limit 0.1 2>/dev/null \
        | awk '/^ *[0-9]+\.[0-9]+%/ {gsub("%","",$1); if ($1+0 > best+0) {best=$1; t=$3}} END {print t}')
  echo "$arm worker tid=$tid"
  perf report --stdio -i "$out/$arm.data" --tid "$tid" --no-children --percentage relative \
    --sort symbol --percent-limit 0.15 >"$out/$arm-symbols.txt" 2>/dev/null
  perf report --stdio -i "$out/$arm.data" --tid "$tid" --no-children --percentage relative \
    --sort dso --percent-limit 0.3 >"$out/$arm-dso.txt" 2>/dev/null
  python3 - "$out/$arm.json" "$arm" <<'PY'
import json,sys
r=json.load(open(sys.argv[1]))
print(f"{sys.argv[2]:6s} tx/s {r['transactions_per_second']:>12,.0f} cyc/tx {r['cycles_per_commit']:>8.1f} ins/tx {r['instructions_per_commit']:>7.0f}")
PY
done
chown -R shuai:shuai "$out" 2>/dev/null
echo done
