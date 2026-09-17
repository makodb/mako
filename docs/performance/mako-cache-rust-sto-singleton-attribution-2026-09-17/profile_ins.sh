#!/usr/bin/env bash
# Instruction-count attribution: perf record -e instructions:u on the frame-pointer probe pair,
# engine mode, 1 worker, filtered to the measured window by PROFILE_WINDOW markers.
set -euo pipefail
OUT=${1:?out dir}
P=/var/tmp/mako-residual-gap.7FPbwg
W=/var/tmp/mako-engine-perf-20260912.iz63eX
mkdir -p "$OUT"
export LD_LIBRARY_PATH="$W:/home/users/shuai/.linuxbrew/opt/llvm@22/lib"
export LD_PRELOAD="$W/libjemalloc.so.2"
for entry in "c-abi:probe-c-abi" "sto:probe-sto"; do
  arm=${entry%%:*}; bin=$P/${entry##*:}
  d="$OUT/ins-$arm"; mkdir -p "$d"
  perf record --clockid mono -e instructions:u -F 3999 --call-graph fp -o "$d/perf.data" -- \
    taskset -c 33-63 "$bin" engine --workers 1 --iterations 16777216 --warmup 1048576 --perf 1 \
    > "$d/stdout.txt" 2> "$d/stderr.txt"
  tid=$(sed -n 's/.*PROFILE_WINDOW begin tid=\([0-9]*\).*/\1/p' "$d/stderr.txt" | head -n1)
  b=$(grep -o 'PROFILE_WINDOW begin.*monotonic_ns=[0-9]*' "$d/stderr.txt" | grep -o '[0-9]*$' | head -n1)
  e=$(grep -o 'PROFILE_WINDOW end.*monotonic_ns=[0-9]*' "$d/stderr.txt" | grep -o '[0-9]*$' | head -n1)
  span="$(python3 -c "print(f'{$b//1000000000}.{$b%1000000000:09}')"),$(python3 -c "print(f'{$e//1000000000}.{$e%1000000000:09}')")"
  echo "$arm tid=$tid span=$span"
  perf report --stdio --tid "$tid" --time "$span" --percentage relative \
    -i "$d/perf.data" --percent-limit 0.15 --no-children --call-graph none --sort symbol \
    > "$d/symbols.txt" 2>/dev/null
  perf report --stdio --tid "$tid" --time "$span" --percentage relative \
    -i "$d/perf.data" --percent-limit 0.5 --children --call-graph graph,0.5,caller --sort symbol \
    > "$d/callchains.txt" 2>/dev/null
  echo "$arm reported"
done
