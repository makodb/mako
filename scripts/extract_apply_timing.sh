#!/bin/bash
# Extracts per-thread-count APPLY-TIMING evidence from a single-Raft sweep.
# Usage: bash scripts/extract_apply_timing.sh <sweep_dir>
# Produces a table with: threads, follower, steady_mean_us, peak_queue, window_eps, committed_eps (= replay_batch/30).

set -e

dir="${1:?usage: $0 <sweep_dir>}"
logs="$dir/logs"
csv="$dir/results.csv"

[ -d "$logs" ] || { echo "no logs dir: $logs" >&2; exit 1; }
[ -f "$csv" ] || { echo "no results.csv: $csv" >&2; exit 1; }

printf '%-6s  %-8s  %-10s  %-12s  %-12s  %-14s  %-10s\n' \
  "threads" "follower" "mean_us" "min_us" "max_us" "peak_backlog" "window_eps"

for f in "$logs"/t*_run1_follower_p1.log "$logs"/t*_run1_follower_p2.log; do
  [ -f "$f" ] || continue
  base=$(basename "$f")
  threads=$(echo "$base" | sed -n 's/^t\([0-9]*\)_run.*/\1/p')
  follower=$(echo "$base" | sed -n 's/.*_\(p[12]\)\.log$/\1/p')

  # Pick the LAST APPLY-TIMING line (steady state).
  # For multi-Raft the instrumentation is per partition — sum across the last
  # APPLY-TIMING line of each partition to get the aggregate picture.
  last_lines=$(grep "APPLY-TIMING" "$f" | awk '
    {
      # Extract par id if present (single-Raft has no par, use 0)
      par = 0
      for (i=1;i<=NF;i++) if ($i=="par") { par = $(i+1); }
      line[par] = $0
    }
    END { for (p in line) print line[p] }
  ')
  [ -z "$last_lines" ] && continue

  # Aggregate across partitions: sum counts, weighted-avg mean, sum peak, sum eps.
  echo "$last_lines" | awk -v t="$threads" -v f="$follower" '
    {
      # Parse "field=value" tokens.
      cnt=0; mean=0; mn=0; mx=0; peak=0; eps=0
      for (i=1;i<=NF;i++) {
        split($i, kv, "=")
        if (kv[1]=="count")     cnt=kv[2]+0
        if (kv[1]=="mean_us")   mean=kv[2]+0
        if (kv[1]=="min_us")    mn=kv[2]+0
        if (kv[1]=="max_us")    mx=kv[2]+0
        if (kv[1]=="peak_queue"   || kv[1]=="peak_backlog") peak=kv[2]+0
        if (kv[1]=="window_eps") eps=kv[2]+0.0
      }
      SUM_CNT   += cnt
      SUM_MEAN_W += mean * cnt
      MIN_US = (NR==1 || mn < MIN_US) ? mn : MIN_US
      MAX_US = (mx > MAX_US) ? mx : MAX_US
      SUM_PEAK += peak
      SUM_EPS  += eps
    }
    END {
      m = SUM_CNT>0 ? int(SUM_MEAN_W / SUM_CNT) : 0
      printf "%-6s  %-8s  %-10s  %-12s  %-12s  %-14s  %-10.1f\n", t, f, m, MIN_US, MAX_US, SUM_PEAK, SUM_EPS
    }
  '
done | sort -n -k1

echo
echo "---- Cross-check: committed-batch rate from results.csv (replay_batch_p1 / 30 s) ----"
awk -F, 'NR>1 && $1 ~ /^[0-9]+$/ { printf "  threads=%s  committed_batches_per_sec=%.1f  (replay_batch_p1=%s, honest ops/s=%.0f)\n", $1, $10/30.0, $10, $10*400/30.0 }' "$csv"
