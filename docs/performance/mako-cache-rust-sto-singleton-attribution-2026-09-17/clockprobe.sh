#!/usr/bin/env bash
# Per-thread clock-call counts for both lanes, engine mode, 1 worker.
set -uo pipefail
W=/var/tmp/mako-engine-perf-20260912.iz63eX
F=/var/tmp/mako-w1-fixes.ShjYqz
export LD_LIBRARY_PATH="$W:/home/users/shuai/.linuxbrew/opt/llvm@22/lib"
export LD_PRELOAD="$W/libjemalloc.so.2 /var/tmp/clockcount.so"
for v in sto:$F/stage3-thin cpp:$F/cpp-thin; do
  tag=${v%%:*}; bin=${v##*:}
  echo "### $tag engine"
  taskset -c 33-63 "$bin" engine --workers 1 --iterations 262144 --warmup 65536 --perf 1 \
    >/dev/null 2>"/var/tmp/cc-$tag.err"
  grep CLOCKCOUNT "/var/tmp/cc-$tag.err" | sort -t= -k3 -rn | head -n 5
done
echo "### strace syscall summary (engine, both lanes)"
for v in sto:$F/stage3-thin cpp:$F/cpp-thin; do
  tag=${v%%:*}; bin=${v##*:}
  echo "-- $tag"
  LD_PRELOAD="$W/libjemalloc.so.2" strace -f -c -e trace=clock_gettime,gettimeofday,clock_nanosleep,time \
    taskset -c 33-63 "$bin" engine --workers 1 --iterations 65536 --warmup 65536 --perf 0 \
    >/dev/null 2>"/var/tmp/st-$tag.txt" || true
  tail -n 8 "/var/tmp/st-$tag.txt"
done
