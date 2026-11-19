#!/usr/bin/env bash
# Run the rpcbench throughput matrix and print one avg-qps line per trial.
#
#   cmake --build build --target rpcbench
#   scripts/run_rpcbench.sh build/rpcbench before-my-change
#
# Each (mode, trial) pair starts a fresh server, runs one client, and kills the
# server.  Three trials per mode: single-run qps on a shared machine is noise,
# and the spread is the only honest way to see whether a delta is real.  Read
# the SPREAD, not the best number -- an effect smaller than the trial-to-trial
# range is not an effect.
#
# The four modes exercise genuinely different dispatch paths, so a change can
# easily move one and not the others:
#   fast   inline dispatch on the poll thread, no fiber
#   fiber  stackful fiber per request (the default server path)
#   defer  fiber with a deferred reply
#   async  stackless rusty::Task
set -uo pipefail

if [[ $# -lt 1 || $# -gt 2 ]]; then
    echo "usage: $0 <path-to-rpcbench> [tag]" >&2
    exit 2
fi

BIN="$1"
TAG="${2:-rpcbench}"

if [[ ! -x "$BIN" ]]; then
    echo "$0: no rpcbench binary at '$BIN'" >&2
    echo "  build it with: cmake --build build --target rpcbench" >&2
    exit 2
fi

# Overridable so a comparison run can be widened without editing this file.
# Flag meanings are rpcbench's own (tests/rpcbench.cc usage block) -- note that
# -n is SECONDS and -b is PACKET BYTES, which is not what the short names
# suggest.
PORT="${RPCBENCH_PORT:-18848}"
N="${RPCBENCH_N:-10}"          # -n  running seconds
B="${RPCBENCH_B:-10}"          # -b  packet byte size
E="${RPCBENCH_E:-2}"           # -e  epoll instances
O="${RPCBENCH_O:-1000}"        # -o  outgoing requests
W="${RPCBENCH_W:-16}"          # -w  worker threads
T="${RPCBENCH_T:-8}"           # -t  client threads
V="${RPCBENCH_V:-64}"          # -v  vector size, fast_vec mode only
TRIALS="${RPCBENCH_TRIALS:-3}"
# fast_vec is omitted by default: it measures a different workload (vector
# payloads) and is not comparable to the other four.  Add it explicitly with
# RPCBENCH_MODES="fast_vec" when that is what you want to measure.
MODES="${RPCBENCH_MODES:-fast fiber defer async}"

SRVLOG="$(mktemp -t rpcbench-server-XXXXXX.log)"
SRV=""
cleanup() {
    [[ -n "$SRV" ]] && kill "$SRV" 2>/dev/null
    rm -f "$SRVLOG"
}
trap cleanup EXIT INT TERM

echo "### $TAG  on $(hostname)  ($(nproc) cpus, load$(uptime | sed 's/.*load average//'))"
echo "### params: -n $N -b $B -e $E -o $O -w $W -t $T   (${TRIALS} trials/mode)"

for mode in $MODES; do
    # fast_vec refuses to run unless a vector size is supplied, and the other
    # modes refuse to run WITH one.
    VEC=()
    [[ "$mode" == "fast_vec" ]] && VEC=(-v "$V")
    for trial in $(seq 1 "$TRIALS"); do
        "$BIN" -s "127.0.0.1:$PORT" -m "$mode" -e "$E" -w "$W" "${VEC[@]}" \
            >"$SRVLOG" 2>&1 &
        SRV=$!
        sleep 2
        OUT=$("$BIN" -c "127.0.0.1:$PORT" -m "$mode" \
                  -n "$N" -b "$B" -e "$E" -o "$O" -w "$W" -t "$T" "${VEC[@]}" 2>&1 |
              grep 'avg qps' | tail -1)
        kill "$SRV" 2>/dev/null
        wait "$SRV" 2>/dev/null
        SRV=""
        QPS=$(echo "$OUT" | grep -oE '[0-9.]+$')
        printf "%-8s trial %d  avg_qps=%s\n" "$mode" "$trial" "${QPS:-FAILED}"
        # The port is in TIME_WAIT for a moment after the server dies.
        sleep 1
    done
done
