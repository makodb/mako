#!/bin/bash
# One consistent sweep of every perf cell, from ONE build.
#
# The perf doc accumulated numbers from three different states of the
# code, which is exactly how a stale figure survives into a conclusion.
# This regenerates all of them together.
#
# Pinning matches docs/dev/srpc_rpcbench_baseline.md: server core 2,
# clients 4-7, -t 4. Change either and the numbers are not comparable.
set -u
ROOT=${ROOT:-/home/users/shuai/mako/.claude/worktrees/srpc-crate}
CPP=${CPP:-/var/tmp/mako-srpc/tree/build_clang22/rpcbench}
RB=$ROOT/target/release/rbench
OUT=${1:-/tmp/srpc_perf.tsv}
N=${N:-5}
port=24000

qps() { grep -oP 'avg qps: \K[0-9.]+' <<<"$1"; }
free_port() { port=$((port + 1)); echo $port; }

printf 'side\tmode\tdepth\tbytes\tqps\n' > "$OUT"

# --- CLIENT: ours against the real C++ server -------------------------
p=$(free_port)
taskset -c 2 "$CPP" -s 127.0.0.1:$p >/dev/null 2>&1 & srv=$!
sleep 2
for depth in 1 100; do
  for b in 10 100 1024; do
    for m in await block; do
      r=$(timeout 90 taskset -c 4-7 "$RB" -c 127.0.0.1:$p -n $N -t 4 -o $depth -b $b -m $m 2>&1)
      printf 'client\t%s\t%s\t%s\t%s\n' "$m" "$depth" "$b" "$(qps "$r")" >> "$OUT"
    done
    c=$(timeout 90 taskset -c 4-7 "$CPP" -c 127.0.0.1:$p -n $N -t 4 -o $depth -b $b -m fast 2>&1)
    printf 'client\tcpp\t%s\t%s\t%s\n' "$depth" "$b" "$(qps "$c")" >> "$OUT"
  done
done
kill $srv 2>/dev/null; sleep 1

# --- SERVER: the real C++ client against ours -------------------------
for m in inline fiber; do
  for b in 10 100 1024; do
    p=$(free_port)
    taskset -c 2 "$RB" -s 127.0.0.1:$p -m $m >/dev/null 2>&1 & srv=$!
    sleep 2
    r=$(timeout 90 taskset -c 4-7 "$CPP" -c 127.0.0.1:$p -n $N -t 4 -o 100 -b $b -m fast 2>&1)
    printf 'server\t%s\t100\t%s\t%s\n' "$m" "$b" "$(qps "$r")" >> "$OUT"
    kill $srv 2>/dev/null; sleep 1
  done
done

# C++ server as the reference, same harness
for b in 10 100 1024; do
  p=$(free_port)
  taskset -c 2 "$CPP" -s 127.0.0.1:$p >/dev/null 2>&1 & srv=$!
  sleep 2
  r=$(timeout 90 taskset -c 4-7 "$CPP" -c 127.0.0.1:$p -n $N -t 4 -o 100 -b $b -m fast 2>&1)
  printf 'server\tcpp\t100\t%s\t%s\n' "$b" "$(qps "$r")" >> "$OUT"
  kill $srv 2>/dev/null; sleep 1
done

echo "wrote $OUT"
column -t "$OUT"
