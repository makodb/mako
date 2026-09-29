#!/usr/bin/env bash
# Jetpack-format sweep of raft_bench arms (docs/performance/jetpack-comparison-plan.md).
#
#   scripts/raft_perf/jetpack/run_jetpack_sweep.sh OUT_ROOT ROUNDS POINTS arm=builddir...
#
# POINTS is a space-separated list of <workload>:<N>, e.g.
# "rw_1000000:1 rw_1000000:150 rw_zipf_0.8:150". A workload is one of the
# copied config/rw_*.yml files: rw_1000000 (uniform) or rw_zipf_<theta>.
# Each arm is <name>=<build dir relative to this repo>; its files are named
# none_<name>-60c1s5r10p-... so Jetpack's scripts read them as a protocol.
#
# Round k lands in OUT_ROOT/round<k>/log/, Jetpack's result-dir layout. Within
# a round every point runs every arm, starting from arm (k + point) mod K, so
# each arm takes every position equally often. WAN_DELAY_MS (default 0) is
# injected by libnetdelay.so and recorded in every run's .res line and in its
# <prefix>.sidecar.json.
set -euo pipefail
OUT_ROOT="$1"; ROUNDS="$2"; POINTS="$3"; shift 3
ARMS=("$@")
K=${#ARMS[@]}
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$HERE/../../.." && pwd)"
WAN="${WAN_DELAY_MS:-0}"
DUR="${JETPACK_DURATION:-30}"   # Jetpack's default run length
SHIM="$HERE/libnetdelay.so"
if [ ! -f "$SHIM" ] || [ "$HERE/netdelay.c" -nt "$SHIM" ]; then
  cc -O2 -Wall -shared -fPIC -o "$SHIM" "$HERE/netdelay.c" -ldl -lpthread
fi
mkdir -p "$OUT_ROOT"

commit_of() {  # the source commit of a build dir's tree
  git -C "$REPO_ROOT/$1/.." rev-parse HEAD 2>/dev/null || echo unknown
}

run_point() {
  local round="$1" arm="$2" dir="$3" workload="$4" n="$5"
  local log="$OUT_ROOT/round$round/log"
  mkdir -p "$log"
  local prefix="none_${arm}-60c1s5r10p-${workload}-concurrent_${n}-0-YCSB_A"
  if [ -s "$log/$prefix-server0.res" ]; then return 0; fi   # resumable
  local zipf=0
  case "$workload" in
    rw_zipf_*) zipf="${workload#rw_zipf_}" ;;
    rw_1000000) ;;
    *) echo "unknown workload $workload" >&2; return 2 ;;
  esac
  local preload=()
  if awk -v w="$WAN" 'BEGIN { exit !(w > 0) }'; then
    preload=(LD_PRELOAD="$SHIM")
  fi
  local start
  start="$(date -u +%FT%TZ)"
  if ! env "${preload[@]}" WAN_DELAY_MS="$WAN" JETPACK_N="$n" JETPACK_ZIPF="$zipf" \
      JETPACK_OUT_DIR="$log" JETPACK_PREFIX="$prefix" \
      MAKO_BENCH_COMMIT="$(commit_of "$dir")" \
      "$REPO_ROOT/examples/raft_bench.sh" --build-dir "$dir" \
      --out "$log/$prefix.bench.json" --partitions 1 --payload-bytes 64 \
      --duration-sec "$DUR" --warmup-sec 0 > "$log/$prefix.run.log" 2>&1; then
    echo "  FAILED: round $round $arm $workload N=$n (see $log/$prefix.run.log)"
    rm -f "$log/$prefix"-server*.res "$log/$prefix"-server*.csv
    return 0
  fi
  cat > "$log/$prefix.sidecar.json" <<EOF
{
  "arm": "$arm",
  "build_dir": "$dir",
  "commit": "$(commit_of "$dir")",
  "tree_changes": "$(git -C "$REPO_ROOT/$dir/.." status --porcelain --untracked-files=no -- src | tr '\n' ' ')",
  "wan_delay_ms": $WAN,
  "injected_rtt_ms": $WAN,
  "delay_model": "each outbound (connect()ed) socket's bytes are sent WAN_DELAY_MS late; replies are not delayed, as Jetpack's WAN_WAIT before each Raft request",
  "n_concurrent": $n,
  "workload": "$workload",
  "zipf_theta": $zipf,
  "ycsb": "YCSB_A",
  "sites": 60,
  "max_undone": 300,
  "duration_s": $DUR,
  "round": $round,
  "started_utc": "$start",
  "host": "$(hostname)"
}
EOF
  echo "  ok: round $round $arm $workload N=$n $(grep -h 'Mid throughput' "$log/$prefix-server0.res")"
}

read -r -a PTS <<< "$POINTS"
for r in $(seq 1 "$ROUNDS"); do
  echo "round $r (WAN_DELAY_MS=$WAN)"
  p=0
  for pt in "${PTS[@]}"; do
    workload="${pt%%:*}"; n="${pt##*:}"
    for j in $(seq 0 $((K - 1))); do
      a="${ARMS[$(( (r + p + j) % K ))]}"
      run_point "$r" "${a%%=*}" "${a#*=}" "$workload" "$n"
    done
    p=$((p + 1))
  done
done
echo "sweep done: $OUT_ROOT"
