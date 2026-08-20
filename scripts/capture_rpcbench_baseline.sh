#!/bin/bash
# S1 — pinned C++ rpcbench baselines for the Goal-1 perf gate.
#
# The gate is "parity against MEASURED baselines", and no pinned baseline
# existed: the only numbers in the repo are unpinned and from a previous
# kernel. This captures one, with the variables fixed and recorded.
#
# Decisions frozen here (change them and the numbers are not comparable):
#   * PINNING — server and client on distinct cores of NUMA node 0, which
#     is one of the two nodes on this 2990WX with locally-attached memory.
#     Server gets one core because the C++ server is a single poll thread
#     pegged at ~100% of one core; that is the shape being measured.
#   * SAMPLING — `-n N` prints one sample per second and DISCARDS the
#     first, so N seconds yields N-1 samples. N=8 gives 7.
#   * COUNTING — left exactly as the C++ harness does it (callback mode
#     counts successful SENDS, await mode counts OK RESPONSES). The Rust
#     side must mirror the same rule rather than "fix" it.
#   * BUILD — as-shipped: C++ is -O2 -march=native + jemalloc. Recorded
#     rather than matched, so the asymmetry is visible when Rust lands.
set -u

BUILD=/var/tmp/mako-srpc/tree/build_clang22
OUT=${1:-/var/tmp/mako-srpc/segv/bench/baseline.tsv}
SRV_CORE=2
CLI_CORES=4-7
PORT_BASE=19300
SECONDS_PER_RUN=8      # -> 7 samples
TRIALS=3

modes="fast fiber defer async"
depths="1 100"
sizes="10 100 1024"

echo -e "mode\tdepth\tbytes\tthreads\ttrial\tavg_qps\tsamples" > "$OUT"

port=$PORT_BASE
for mode in $modes; do
  for depth in $depths; do
    for bytes in $sizes; do
      for trial in $(seq 1 $TRIALS); do
        port=$((port + 1))
        taskset -c $SRV_CORE "$BUILD/rpcbench" -s "127.0.0.1:$port" \
          >/tmp/bl_srv.log 2>&1 &
        srv=$!
        sleep 1.5
        if ! kill -0 $srv 2>/dev/null; then
          echo "server died before $mode/$depth/$bytes trial $trial" >&2
          continue
        fi
        cli=$(taskset -c $CLI_CORES "$BUILD/rpcbench" -c "127.0.0.1:$port" \
                -n $SECONDS_PER_RUN -t 4 -o "$depth" -b "$bytes" -m "$mode" 2>&1)
        avg=$(echo "$cli" | grep -oP 'avg qps: \K[0-9.]+' | tail -1)
        n=$(echo "$cli" | grep -c 'qps: [0-9]')
        [ -z "$avg" ] && avg=NA
        echo -e "$mode\t$depth\t$bytes\t4\t$trial\t$avg\t$n" >> "$OUT"
        kill $srv 2>/dev/null
        wait $srv 2>/dev/null
        sleep 0.3
      done
    done
  done
done
echo "BASELINE-DONE -> $OUT"
